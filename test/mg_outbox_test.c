/*
 * mg_outbox_test.c —— 发送队列的掉电一致性
 *
 * 这里的重点不是「能入队能出队」，而是**在任意一步掉电之后，读回来的东西是什么**。
 * 所以用一块内存模拟 Flash 扇区，能精确控制「第几次写之后断电」。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mg_outbox.h"
#include "mg_outbox_file.h"

#define RAM_SIZE 8192

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

static void check_u32(uint32_t got, uint32_t want, const char *what)
{
    g_total++;
    if (got != want)
    {
        g_fail++;
        printf("  [FAIL] %s  (got %u, want %u)\n", what, (unsigned)got, (unsigned)want);
    }
}

/* ---------- 内存后端：可以指定「写多少次之后断电」 ---------- */

typedef struct
{
    uint8_t mem[RAM_SIZE];
    uint32_t size;
    int writes;
    int cut_after; /* >0：第 cut_after 次写之后全部失败，模拟掉电 */
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

static void ram_io(ram_t *r, mg_outbox_io_t *io)
{
    io->read = ram_read;
    io->write = ram_write;
    io->ctx = r;
}

/* ---------- 用例 ---------- */

static void test_empty(void)
{
    ram_t ram;
    mg_outbox_io_t io;
    mg_outbox_t ob;
    uint32_t seq = 0;
    uint16_t len = 0;
    uint8_t buf[MG_OUTBOX_PAYLOAD_MAX];

    printf("[空扇区]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);

    check(mg_outbox_init(&ob, &io, 4) == 0, "init on blank sector succeeds");
    check_u32(mg_outbox_pending(&ob), 0, "blank sector -> nothing pending");
    check(mg_outbox_peek(&ob, &seq, buf, &len) == 0, "peek on empty returns 0");
    check(ob.st.resync == 1, "blank sector is treated as a resync");
}

static void test_fifo(void)
{
    ram_t ram;
    mg_outbox_io_t io;
    mg_outbox_t ob;
    uint32_t seq;
    uint16_t len;
    uint8_t buf[MG_OUTBOX_PAYLOAD_MAX];

    printf("[顺序与确认]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);
    mg_outbox_init(&ob, &io, 4);

    check(mg_outbox_put(&ob, (const uint8_t *)"a", 1) == 0, "put a");
    check(mg_outbox_put(&ob, (const uint8_t *)"bb", 2) == 0, "put b");
    check(mg_outbox_put(&ob, (const uint8_t *)"ccc", 3) == 0, "put c");
    check_u32(mg_outbox_pending(&ob), 3, "three records pending");

    check(mg_outbox_peek(&ob, &seq, buf, &len) == 1, "peek returns one");
    check_u32(seq, 0, "first record has the lowest sequence");
    check(len == 1 && memcmp(buf, "a", 1) == 0, "first payload is 'a'");

    /* 不确认就再 peek 一次，必须还是同一条 —— 否则等于静默丢数据 */
    check(mg_outbox_peek(&ob, &seq, buf, &len) == 1 && seq == 0 && memcmp(buf, "a", 1) == 0,
          "peek without commit returns the same record");

    check(mg_outbox_commit(&ob, seq) == 0, "commit the head");
    check_u32(mg_outbox_pending(&ob), 2, "pending drops by one");
    check(mg_outbox_peek(&ob, &seq, buf, &len) == 1 && seq == 1 && memcmp(buf, "bb", 2) == 0,
          "next record follows in order");

    /* 重复确认是无害的，不能把队列再往前推一格 */
    check(mg_outbox_commit(&ob, 0) == 0, "duplicate commit is harmless");
    check_u32(mg_outbox_pending(&ob), 2, "duplicate commit does not advance");

    /* 乱序确认必须拒绝：放过去等于跳过中间还没发的那条 */
    check(mg_outbox_commit(&ob, 3) != 0, "out-of-order commit is rejected");
    check_u32(mg_outbox_pending(&ob), 2, "rejected commit leaves the queue alone");
}

static void test_overflow(void)
{
    ram_t ram;
    mg_outbox_io_t io;
    mg_outbox_t ob;
    uint32_t seq;
    uint16_t len;
    uint8_t buf[MG_OUTBOX_PAYLOAD_MAX];
    char msg[16];

    printf("[满环丢最旧]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);
    mg_outbox_init(&ob, &io, 4);

    for (int i = 0; i < 6; i++)
    {
        snprintf(msg, sizeof(msg), "m%d", i);
        check(mg_outbox_put(&ob, (const uint8_t *)msg, (uint16_t)strlen(msg)) == 0, "put");
    }

    check_u32(mg_outbox_pending(&ob), 4, "pending is capped by the slot count");
    check_u32(ob.st.dropped, 2, "two oldest records were dropped");

    check(mg_outbox_peek(&ob, &seq, buf, &len) == 1, "peek still works");
    check_u32(seq, 2, "head is now the third record");
    check(len == 2 && memcmp(buf, "m2", 2) == 0, "payload of the new head");
}

static void test_reject_too_long(void)
{
    ram_t ram;
    mg_outbox_io_t io;
    mg_outbox_t ob;
    uint8_t big[MG_OUTBOX_PAYLOAD_MAX + 1];

    printf("[超长拒收]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);
    mg_outbox_init(&ob, &io, 4);

    memset(big, 'x', sizeof(big));
    check(mg_outbox_put(&ob, big, sizeof(big)) == -1, "payload over the limit is rejected");
    check_u32(ob.st.rejected, 1, "rejection is counted");
    check_u32(mg_outbox_pending(&ob), 0, "nothing entered the queue");
}

/*
 * 掉电点 1：状态字节还没写。
 * 前面三步（失效、写头、写载荷）都成功了，最后一步失败。
 * 读回来必须看不到这条 —— 否则会发出一条载荷是半截的消息。
 */
static void test_torn_before_commit(void)
{
    ram_t ram;
    mg_outbox_io_t io;
    mg_outbox_t ob;

    printf("[掉电：提交字节未写]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);
    mg_outbox_init(&ob, &io, 4);

    ram.writes = 0;
    ram.cut_after = 3; /* put 一共写 4 次：失效、头、载荷、状态 */
    check(mg_outbox_put(&ob, (const uint8_t *)"half", 4) == -1, "put fails when power dies");
    ram.cut_after = 0;

    /* 重新上电 */
    check(mg_outbox_init(&ob, &io, 4) == 0, "reinit succeeds");
    check_u32(mg_outbox_pending(&ob), 0, "the torn record is not visible");
}

/*
 * 掉电点 2：状态字节写了，但载荷在存储里被写坏（老化、坏块）。
 * 这种情况 CRC 必须挡住 —— 否则会把乱码当业务数据发出去。
 */
static void test_corrupt_payload(void)
{
    ram_t ram;
    mg_outbox_io_t io;
    mg_outbox_t ob;
    uint32_t seq;
    uint16_t len;
    uint8_t buf[MG_OUTBOX_PAYLOAD_MAX];

    printf("[坏块：载荷被改写]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);
    mg_outbox_init(&ob, &io, 4);

    mg_outbox_put(&ob, (const uint8_t *)"good", 4);
    mg_outbox_put(&ob, (const uint8_t *)"also-good", 9);

    /* 把第一条的载荷改一个字节 */
    ram.mem[MG_OUTBOX_HDR_SIZE] ^= 0xFF;
    check(mg_outbox_init(&ob, &io, 4) == 0, "reinit succeeds");

    check(mg_outbox_peek(&ob, &seq, buf, &len) == 1, "peek skips the corrupted head");
    check_u32(seq, 1, "the surviving record keeps its sequence");
    check(len == 9 && memcmp(buf, "also-good", 9) == 0, "payload of the surviving record");
    check(ob.st.dropped >= 1, "the skipped record is counted");
}

/*
 * 掉电点 3：头部的 read_seq 丢了。
 *
 * 只能从槽位反推「最旧的一条」。代价是已经确认过的那条会被重发一次 ——
 * 这是刻意的取舍：宁可重复，不可丢失。
 */
static void test_header_lost(void)
{
    ram_t ram;
    mg_outbox_io_t io;
    mg_outbox_t ob;
    uint32_t seq;
    uint16_t len;
    uint8_t buf[MG_OUTBOX_PAYLOAD_MAX];

    printf("[掉电：头部丢失]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);
    mg_outbox_init(&ob, &io, 4);

    mg_outbox_put(&ob, (const uint8_t *)"one", 3);
    mg_outbox_put(&ob, (const uint8_t *)"two", 3);
    mg_outbox_commit(&ob, 0); /* 第一条已送达 */

    memset(ram.mem, 0, MG_OUTBOX_HDR_SIZE); /* 头部掉了 */

    check(mg_outbox_init(&ob, &io, 4) == 0, "reinit succeeds");
    check_u32(ob.st.resync, 1, "resync is counted");
    check_u32(mg_outbox_pending(&ob), 2, "both records are pending again (one duplicated)");
    check(mg_outbox_peek(&ob, &seq, buf, &len) == 1, "peek returns the oldest record");
    check_u32(seq, 0, "the confirmed record is replayed, not skipped");
}

static void test_restart(void)
{
    ram_t ram;
    mg_outbox_io_t io;
    mg_outbox_t ob;
    uint32_t seq;
    uint16_t len;
    uint8_t buf[MG_OUTBOX_PAYLOAD_MAX];

    printf("[重启后继续]\n");
    ram_reset(&ram);
    ram_io(&ram, &io);
    mg_outbox_init(&ob, &io, 4);

    mg_outbox_put(&ob, (const uint8_t *)"r0", 2);
    mg_outbox_put(&ob, (const uint8_t *)"r1", 2);
    mg_outbox_commit(&ob, 0);
    mg_outbox_commit(&ob, 1);
    mg_outbox_put(&ob, (const uint8_t *)"r2", 2);

    /* 重新上电：头部有效，走正常恢复路径，不触发 resync */
    check(mg_outbox_init(&ob, &io, 4) == 0, "reinit succeeds");
    check_u32(ob.st.resync, 0, "a clean shutdown does not force a resync");
    check_u32(mg_outbox_pending(&ob), 1, "only the unsent record is pending");
    check(mg_outbox_peek(&ob, &seq, buf, &len) == 1, "peek returns it");
    check_u32(seq, 2, "sequence continues across restarts");
    check(len == 2 && memcmp(buf, "r2", 2) == 0, "payload survives the restart");
}

static void test_file_backend(void)
{
    const char *path = ".outbox_test.bin";
    mg_outbox_file_t f;
    mg_outbox_io_t io;
    mg_outbox_t ob;
    uint32_t seq;
    uint16_t len;
    uint8_t buf[MG_OUTBOX_PAYLOAD_MAX];
    uint32_t size = MG_OUTBOX_HDR_SIZE + 4 * MG_OUTBOX_SLOT_SIZE;

    printf("[文件后端]\n");
    unlink(path);

    check(mg_outbox_file_open(&f, path, size) == 0, "open creates the file");
    mg_outbox_file_io(&f, &io);
    check(mg_outbox_init(&ob, &io, 4) == 0, "init on a new file succeeds");
    check(mg_outbox_put(&ob, (const uint8_t *)"disk", 4) == 0, "put to file");
    mg_outbox_file_close(&f);

    /* 换个句柄重新打开，模拟进程重启 */
    check(mg_outbox_file_open(&f, path, size) == 0, "reopen succeeds");
    mg_outbox_file_io(&f, &io);
    check(mg_outbox_init(&ob, &io, 4) == 0, "init after reopen succeeds");
    check_u32(mg_outbox_pending(&ob), 1, "record survived the process restart");
    check(mg_outbox_peek(&ob, &seq, buf, &len) == 1 && len == 4 &&
              memcmp(buf, "disk", 4) == 0,
          "payload survived the process restart");
    mg_outbox_file_close(&f);

    unlink(path);
}

int main(void)
{
    printf("=== mg_outbox_test ===\n");

    test_empty();
    test_fifo();
    test_overflow();
    test_reject_too_long();
    test_torn_before_commit();
    test_corrupt_payload();
    test_header_lost();
    test_restart();
    test_file_backend();

    printf("\nResult: %d checks, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
