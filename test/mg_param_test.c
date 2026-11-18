/*
 * mg_param_test.c —— 参数区的编码与双页存储
 *
 * 关心的是「掉电/坏块之后读回来的是哪一份」：
 * 两页交替，任何时刻至少一页完整；坏的那一份不能被当成新参数用。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mg_param.h"
#include "mg_param_file.h"

#define RAM_SIZE 1024

static int g_total;
static int g_fail;

static void check(int cond, const char *what)
{
    g_total++;
    if (!cond)
    {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

typedef struct
{
    uint8_t mem[RAM_SIZE];
    uint32_t size;
    int writes;
    int cut_after; /* >0：第 cut_after 次写之后全部失败 */
} ram_t;

static int ram_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    ram_t *r = (ram_t *)ctx;

    if (off + n > r->size)
        return -1;
    memcpy(buf, r->mem + off, n);
    return 0;
}

static int ram_write(void *ctx, uint32_t off, const void *buf, uint32_t n)
{
    ram_t *r = (ram_t *)ctx;

    if (off + n > r->size)
        return -1;
    r->writes++;
    if (r->cut_after > 0 && r->writes > r->cut_after)
        return -1;
    memcpy(r->mem + off, buf, n);
    return 0;
}

static void ram_reset(ram_t *r)
{
    memset(r, 0, sizeof(*r));
    r->size = RAM_SIZE;
}

static void ram_io(ram_t *r, mg_storage_io_t *io)
{
    io->read = ram_read;
    io->write = ram_write;
    io->ctx = r;
}

/* ---------- 编解码与校验 ---------- */

static void test_codec(void)
{
    mg_param_t a, b;
    uint8_t buf[MG_PARAM_PAYLOAD_MAX];
    uint16_t len = 0;

    printf("[编解码]\n");
    mg_param_defaults(&a);

    check(mg_param_validate(&a) == 0, "defaults are valid");
    check(mg_param_encode(&a, buf, sizeof(buf), &len) == 0, "encode succeeds");
    check(len > 0 && len < sizeof(mg_param_t),
          "wire size is smaller than the struct (no memory layout dumped)");

    check(mg_param_decode(&b, buf, len) == 0, "decode succeeds");
    check(b.ch_count == a.ch_count && b.poll_count == a.poll_count, "counts round-trip");
    check(strcmp(b.ch[0].name, a.ch[0].name) == 0, "channel name round-trips");
    check(strcmp(b.ch[0].arg, a.ch[0].arg) == 0, "channel arg round-trips");
    check(b.poll[3].slave == a.poll[3].slave && b.poll[3].reg == a.poll[3].reg,
          "poll entry round-trips");
    check(strcmp(b.mqtt_addr, a.mqtt_addr) == 0, "mqtt address round-trips");

    /* 截断必须被拒绝，不能「读到多少算多少」 */
    check(mg_param_decode(&b, buf, (uint16_t)(len - 1)) != 0, "truncated payload is rejected");

    /* 版本号不对：固件升级后旧数据不能被当成新参数 */
    {
        uint8_t bad[MG_PARAM_PAYLOAD_MAX];

        memcpy(bad, buf, len);
        bad[0] = 0x7F;
        check(mg_param_decode(&b, bad, len) != 0, "unknown version is rejected");
    }

    /* 多余的非零尾字节说明写入方比读取方新 */
    {
        uint8_t more[MG_PARAM_PAYLOAD_MAX];

        memcpy(more, buf, len);
        more[len] = 0xAB;
        check(mg_param_decode(&b, more, (uint16_t)(len + 1)) != 0,
              "unknown trailing bytes are rejected");
    }
}

static void test_validate(void)
{
    mg_param_t p;

    printf("[字段校验]\n");
    mg_param_defaults(&p);
    check(mg_param_validate(&p) == 0, "baseline valid");

    p.ch_count = 0;
    check(mg_param_validate(&p) != 0, "no channel is rejected");
    mg_param_defaults(&p);

    p.poll_count = 0;
    check(mg_param_validate(&p) != 0, "no poll entry is rejected");
    mg_param_defaults(&p);

    p.poll[0].ch = 3;
    check(mg_param_validate(&p) != 0, "poll entry on a missing channel is rejected");
    mg_param_defaults(&p);

    p.poll[0].slave = 0;
    check(mg_param_validate(&p) != 0, "slave 0 (broadcast) is rejected for polling");
    mg_param_defaults(&p);

    p.poll[0].slave = 248;
    check(mg_param_validate(&p) != 0, "reserved slave address is rejected");
    mg_param_defaults(&p);

    p.poll[0].func = 5;
    check(mg_param_validate(&p) != 0, "unsupported function code is rejected");
    mg_param_defaults(&p);

    p.poll[0].count = 0;
    check(mg_param_validate(&p) != 0, "zero register count is rejected");
    mg_param_defaults(&p);

    p.poll[0].reg = 0xFFFF;
    p.poll[0].count = 2;
    check(mg_param_validate(&p) != 0, "register range overflows 16 bits");
    mg_param_defaults(&p);

    p.poll_interval_ms = 0;
    check(mg_param_validate(&p) != 0, "zero poll interval is rejected");
    mg_param_defaults(&p);

    p.session_timeout_ms = 60001;
    check(mg_param_validate(&p) != 0, "absurd timeout is rejected");
}

/* ---------- 双页存储 ---------- */

static void test_blank(void)
{
    ram_t ram;
    mg_storage_io_t io;
    mg_param_t def, out;
    int page = 123;

    printf("[空扇区]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);
    mg_param_defaults(&def);

    check(mg_param_load(&out, &io, &def, &page) == 1, "blank sector reports 'no data'");
    check(page == -1, "no active page on a blank sector");
    check(mg_param_active_page(&io) == -1, "active page is -1 when nothing is stored");
    check(out.ch_count == def.ch_count, "fallback is the default set");
}

static void test_roundtrip(void)
{
    ram_t ram;
    mg_storage_io_t io;
    mg_param_t p, out;
    int page = -1;

    printf("[保存与恢复]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);

    mg_param_defaults(&p);
    check(mg_param_save(&p, &io) == 0, "first save succeeds");
    check(mg_param_active_page(&io) == 1, "first save lands on page 1");

    check(mg_param_load(&out, &io, 0, &page) == 0, "load after save succeeds");
    check(page == 1, "load reports page 1");
    check(out.poll_count == p.poll_count, "poll table survives");

    /* 改一个字段再存，应该落到另一页 */
    p.poll_interval_ms = 250;
    check(mg_param_save(&p, &io) == 0, "second save succeeds");
    check(mg_param_active_page(&io) == 0, "second save lands on page 0");

    check(mg_param_load(&out, &io, 0, &page) == 0, "load after second save succeeds");
    check(page == 0, "load reports page 0");
    check(out.poll_interval_ms == 250, "newest value wins");

    /* 第三次又回到页 1 */
    p.poll_interval_ms = 300;
    check(mg_param_save(&p, &io) == 0, "third save succeeds");
    check(mg_param_active_page(&io) == 1, "third save lands on page 1 again");
    check(mg_param_load(&out, &io, 0, 0) == 0 && out.poll_interval_ms == 300,
          "alternation keeps the newest value");
}

/*
 * 新页坏了（写一半掉电 / 坏块）：必须回落到旧页，而不是回默认值。
 * 这是双页设计存在的唯一理由，也是最容易被写成「坏了就回默认」的地方。
 */
static void test_corrupt_newest(void)
{
    ram_t ram;
    mg_storage_io_t io;
    mg_param_t p, out;
    int page = -1;

    printf("[新页损坏]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);

    mg_param_defaults(&p);
    p.poll_interval_ms = 111;
    mg_param_save(&p, &io); /* 页 1 */

    p.poll_interval_ms = 222;
    mg_param_save(&p, &io); /* 页 0，当前有效 */

    /* 破坏页 0 的负载首字节 */
    ram.mem[MG_PARAM_HDR_SIZE + 8] ^= 0xFF;

    check(mg_param_active_page(&io) == 1, "corrupted page 0 is skipped");
    check(mg_param_load(&out, &io, 0, &page) == 0, "load falls back to page 1");
    check(page == 1, "fallback reports page 1");
    check(out.poll_interval_ms == 111, "fallback carries the previous value, not defaults");
}

static void test_torn_save(void)
{
    ram_t ram;
    mg_storage_io_t io;
    mg_param_t p, out;

    printf("[保存时掉电]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);

    mg_param_defaults(&p);
    p.poll_interval_ms = 120;
    check(mg_param_save(&p, &io) == 0, "baseline save succeeds");

    p.poll_interval_ms = 999;
    ram.writes = 0;
    ram.cut_after = 2; /* 负载 + 让页头失效之后断电，magic 没写上 */
    check(mg_param_save(&p, &io) != 0, "save reports failure when power dies");
    ram.cut_after = 0;

    check(mg_param_load(&out, &io, 0, 0) == 0, "load still succeeds after a torn save");
    check(out.poll_interval_ms == 120, "the old value survives a torn save");
}

static void test_invalid_rejected(void)
{
    ram_t ram;
    mg_storage_io_t io;
    mg_param_t p, out;

    printf("[保存前校验]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);

    mg_param_defaults(&p);
    p.poll_interval_ms = 400;
    mg_param_save(&p, &io);

    p.poll[0].slave = 0; /* 非法 */
    check(mg_param_save(&p, &io) != 0, "invalid parameters are not written");
    check(mg_param_load(&out, &io, 0, 0) == 0 && out.poll_interval_ms == 400,
          "storage keeps the last valid set");
    check(out.poll[0].slave == 1, "the invalid field never reached the storage");
}

static void test_file_backend(void)
{
    const char *path = ".param_test.bin";
    mg_param_t p, out;
    int page = -1;

    printf("[文件后端]\n");
    unlink(path);

    mg_param_defaults(&p);
    p.poll_interval_ms = 175;

    check(mg_param_load_file(path, &out, &page) == 0, "first load creates defaults");
    check(page == -1, "first load reports defaults");
    check(out.poll_interval_ms == 100, "defaults are used when the file has no parameter");

    check(mg_param_save_file(path, &p) == 0, "save to file succeeds");
    check(mg_param_load_file(path, &out, &page) == 0, "load from file succeeds");
    check(page >= 0, "an active page exists now");
    check(out.poll_interval_ms == 175, "value round-trips through the file");

    unlink(path);
}

int main(void)
{
    printf("=== mg_param_test ===\n");

    test_codec();
    test_validate();
    test_blank();
    test_roundtrip();
    test_corrupt_newest();
    test_torn_save();
    test_invalid_rejected();
    test_file_backend();

    printf("\nResult: %d checks, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
