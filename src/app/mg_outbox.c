/*
 * mg_outbox.c —— 掉电不丢的发送队列
 *
 * 布局：
 *   [头部 16B][槽 0][槽 1] ... [槽 N-1]，槽大小固定 216B（16B 头 + 200B 载荷）
 *
 *  头部：magic(4) version(1) 保留(3) read_seq(4) crc16(2) 保留(2)
 *  槽：  状态(1) 长度(1) 保留(2) 序号(4) crc16(2) 保留(6) 载荷(200)
 *
 * 用「序号取模」定位槽位，环形复用；read_seq 持久化在头部，
 * write_seq 不持久化——它可以从槽里的最大序号推出来。
 * 少存一个字段就少一个「两个指针互相矛盾」的失效模式。
 */
#include "mg_outbox.h"

#include <string.h>

#define OUTBOX_VERSION 1u

static uint8_t g_slot_buf[MG_OUTBOX_PAYLOAD_MAX];

/* ---------- 小工具 ---------- */

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/*
 * CRC-16/CCITT-FALSE。
 *
 * 和 Modbus 那个 CRC 不是一回事（多项式与位序都不同），
 * 这里保护的是本机存储，不参与总线报文，所以不复用 mg_modbus 的实现。
 */
static uint16_t crc16_update(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    return crc;
}

static uint16_t crc16_buf(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFFu;

    for (uint16_t i = 0; i < n; i++)
        crc = crc16_update(crc, p[i]);
    return crc;
}

/* 槽头里参与校验的字段（长度、序号）+ 载荷 */
static uint16_t slot_crc(uint16_t len, uint32_t seq, const uint8_t *payload, uint16_t n)
{
    uint8_t head[5];
    uint16_t crc;

    head[0] = (uint8_t)len;
    put_u32(&head[1], seq);

    crc = crc16_buf(head, sizeof(head));
    for (uint16_t i = 0; i < n; i++)
        crc = crc16_update(crc, payload[i]);
    return crc;
}

static uint32_t slot_off(uint16_t idx)
{
    return (uint32_t)MG_OUTBOX_HDR_SIZE + (uint32_t)idx * MG_OUTBOX_SLOT_SIZE;
}

/*
 * 读一个槽并校验。返回值：
 *   1  有效记录（长度与序号通过 out 参数给出，载荷留在 g_slot_buf）
 *   0  空槽，或内容不可信（*was_marked 区分「本来就没写过」和「写了但坏了」）
 *  -1  后端读失败
 */
static int slot_load(mg_outbox_t *ob, uint16_t idx, uint32_t *seq, uint16_t *len,
                     int *was_marked)
{
    uint8_t hdr[MG_OUTBOX_SLOT_HDR];
    uint32_t off = slot_off(idx);
    uint16_t n, want;
    uint32_t s;

    if (was_marked != 0)
        *was_marked = 0;

    if (ob->io.read(ob->io.ctx, off, hdr, MG_OUTBOX_SLOT_HDR) != 0)
        return -1;

    if (hdr[0] != MG_OUTBOX_STATE_VALID)
        return 0;

    if (was_marked != 0)
        *was_marked = 1;

    n = hdr[1];
    s = get_u32(&hdr[4]);
    want = (uint16_t)(hdr[8] | (hdr[9] << 8));

    if (n > MG_OUTBOX_PAYLOAD_MAX)
        return 0;
    /* 序号必须落在自己这一格上，否则是上一圈的残留 */
    if ((s % ob->slots) != idx)
        return 0;

    if (n > 0 && ob->io.read(ob->io.ctx, off + MG_OUTBOX_SLOT_HDR, g_slot_buf, n) != 0)
        return -1;

    if (slot_crc(n, s, g_slot_buf, n) != want)
        return 0;

    *seq = s;
    *len = n;
    return 1;
}

static int write_header(mg_outbox_t *ob)
{
    uint8_t hdr[MG_OUTBOX_HDR_SIZE];
    uint16_t crc;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'M';
    hdr[1] = 'G';
    hdr[2] = 'O';
    hdr[3] = 'B';
    hdr[4] = OUTBOX_VERSION;
    put_u32(&hdr[8], ob->read_seq);

    crc = crc16_buf(hdr, 12);
    hdr[12] = (uint8_t)(crc & 0xFF);
    hdr[13] = (uint8_t)(crc >> 8);

    return ob->io.write(ob->io.ctx, 0, hdr, sizeof(hdr));
}

/*
 * 写一个槽。三步的顺序不能改：
 *   1) 状态字节先置 0，把上一轮的残留作废
 *   2) 写长度/序号/CRC/载荷
 *   3) 最后写 0xA5，这一步才是提交
 * 任何一步之前掉电，这条记录都不会被当成有效数据读出来。
 */
static int slot_store(mg_outbox_t *ob, uint16_t idx, uint32_t seq,
                      const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[MG_OUTBOX_SLOT_HDR];
    uint8_t state;
    uint32_t off = slot_off(idx);
    uint16_t crc;

    state = MG_OUTBOX_STATE_EMPTY;
    if (ob->io.write(ob->io.ctx, off, &state, 1) != 0)
        return -1;

    memset(hdr, 0, sizeof(hdr));
    hdr[1] = (uint8_t)len;
    put_u32(&hdr[4], seq);
    crc = slot_crc(len, seq, payload, len);
    hdr[8] = (uint8_t)(crc & 0xFF);
    hdr[9] = (uint8_t)(crc >> 8);

    if (ob->io.write(ob->io.ctx, off + 1, hdr + 1, MG_OUTBOX_SLOT_HDR - 1) != 0)
        return -1;
    if (len > 0 && ob->io.write(ob->io.ctx, off + MG_OUTBOX_SLOT_HDR, payload, len) != 0)
        return -1;

    state = MG_OUTBOX_STATE_VALID;
    if (ob->io.write(ob->io.ctx, off, &state, 1) != 0)
        return -1;

    return 0;
}

/* ---------- 对外接口 ---------- */

int mg_outbox_init(mg_outbox_t *ob, const mg_outbox_io_t *io, uint16_t slots)
{
    uint8_t hdr[MG_OUTBOX_HDR_SIZE];
    uint32_t min_seq = 0, max_seq = 0;
    int have = 0, hdr_ok;

    if (ob == 0 || io == 0 || io->read == 0 || io->write == 0)
        return -1;
    if (slots == 0 || slots > MG_OUTBOX_SLOTS_MAX)
        return -1;

    memset(ob, 0, sizeof(*ob));
    ob->io = *io;
    ob->slots = slots;

    if (io->read(io->ctx, 0, hdr, sizeof(hdr)) != 0)
        return -1;

    ob->ready = 1;

    hdr_ok = (hdr[0] == 'M' && hdr[1] == 'G' && hdr[2] == 'O' && hdr[3] == 'B' &&
              hdr[4] == OUTBOX_VERSION &&
              crc16_buf(hdr, 12) == (uint16_t)(hdr[12] | (hdr[13] << 8)));

    for (uint16_t i = 0; i < slots; i++)
    {
        uint32_t seq = 0;
        uint16_t len = 0;
        int marked = 0;
        int r = slot_load(ob, i, &seq, &len, &marked);

        if (r < 0)
        {
            ob->ready = 0;
            return -1;
        }
        if (r == 0)
        {
            if (marked)
                ob->st.torn++; /* 状态字节写了但内容不对：写到一半掉电 */
            continue;
        }

        if (!have || seq < min_seq)
            min_seq = seq;
        if (!have || seq > max_seq)
            max_seq = seq;
        have = 1;
    }

    if (hdr_ok)
    {
        ob->read_seq = get_u32(&hdr[8]);

        if (have)
        {
            /*
             * 头部指向的那批已经被覆盖、写坏或从来没落盘：
             * 跳到现存最旧的一条，并把中间缺掉的条数计进 dropped。
             * 不计数的话，「丢了 3 条」和「本来就只有 1 条」在日志里长得一样。
             */
            if (ob->read_seq < min_seq)
            {
                ob->st.dropped += min_seq - ob->read_seq;
                ob->read_seq = min_seq;
            }
            /* 头部比记录还新：说明发完了但头部更新晚了一步 */
            if (ob->read_seq > max_seq + 1)
                ob->read_seq = max_seq + 1;
            ob->write_seq = max_seq + 1;
        }
        else
        {
            /* 空队列。序号继续往前走，避免重启后序号回绕造成重复判定错误 */
            ob->write_seq = ob->read_seq;
        }

        /* 防御：窗口不可能超过槽数，超了说明头部与槽位不一致 */
        if ((uint32_t)(ob->write_seq - ob->read_seq) > slots)
            ob->write_seq = ob->read_seq + slots;
    }
    else
    {
        /*
         * 头部不可用（首次使用，或擦写头部时掉电）。
         * 只能从槽位反推读取位置：取现存最旧的一条。
         * 代价是「已发但头部没来得及更新」的那条会被重发一次 ——
         * 宁可重复，不可丢失，这就是 at-least-once 的取舍。
         */
        ob->read_seq = have ? min_seq : 0;
        ob->write_seq = have ? max_seq + 1 : 0;
        ob->st.resync++;

        if (write_header(ob) != 0)
        {
            ob->ready = 0;
            return -1;
        }
    }

    return 0;
}

int mg_outbox_put(mg_outbox_t *ob, const uint8_t *payload, uint16_t len)
{
    uint16_t idx;

    if (ob == 0 || !ob->ready || payload == 0)
        return -1;

    if (len == 0 || len > MG_OUTBOX_PAYLOAD_MAX)
    {
        ob->st.rejected++;
        return -1;
    }

    /*
     * 满环丢最旧。
     *
     * 反过来（拒收新的）会让故障期间整段数据全缺：旧的那批即使发出去
     * 也已经过时，而新数据反映的是当前状态。遥测就该这样取舍。
     */
    if ((uint32_t)(ob->write_seq - ob->read_seq) >= ob->slots)
    {
        ob->read_seq++;
        ob->st.dropped++;
        if (write_header(ob) != 0)
            return -1;
    }

    idx = (uint16_t)(ob->write_seq % ob->slots);
    if (slot_store(ob, idx, ob->write_seq, payload, len) != 0)
    {
        ob->st.rejected++;
        return -1;
    }

    ob->write_seq++;
    ob->st.put++;
    return 0;
}

int mg_outbox_peek(mg_outbox_t *ob, uint32_t *seq, uint8_t *buf, uint16_t *len)
{
    if (ob == 0 || !ob->ready || seq == 0 || buf == 0 || len == 0)
        return -1;

    for (;;)
    {
        uint32_t s = 0;
        uint16_t n = 0;
        uint16_t idx;
        int r;

        if (ob->read_seq == ob->write_seq)
            return 0;

        idx = (uint16_t)(ob->read_seq % ob->slots);
        r = slot_load(ob, idx, &s, &n, 0);

        if (r < 0)
            return -1;

        if (r == 0 || s != ob->read_seq)
        {
            /*
             * 队首那条不见了（被覆盖，或写坏了）。
             * 跳过并计数，不能让 peek 卡在同一条上 —— 卡住的后果是
             * 整条上行链路永久停在那里，比丢几条严重得多。
             */
            ob->read_seq++;
            ob->st.dropped++;
            if (write_header(ob) != 0)
                return -1;
            continue;
        }

        memcpy(buf, g_slot_buf, n);
        *seq = s;
        *len = n;
        return 1;
    }
}

int mg_outbox_commit(mg_outbox_t *ob, uint32_t seq)
{
    if (ob == 0 || !ob->ready)
        return -1;

    if (seq != ob->read_seq)
    {
        /* 已经确认过的重复确认是无害的；比队首还新的是乱序，拒绝 */
        return ((int32_t)(ob->read_seq - seq) > 0) ? 0 : -1;
    }

    ob->read_seq++;
    ob->st.sent++;
    if (write_header(ob) != 0)
        return -1;

    return 0;
}

uint32_t mg_outbox_pending(const mg_outbox_t *ob)
{
    if (ob == 0 || !ob->ready)
        return 0;
    return ob->write_seq - ob->read_seq;
}
