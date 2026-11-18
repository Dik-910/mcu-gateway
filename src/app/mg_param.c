/*
 * mg_param.c —— 参数区的编码与双页存储
 *
 * 两页布局：
 *   [页头 16B][负载 ≤496B]  ×2，整块 1KB
 *
 * 页头：magic(4) seq(4) len(2) crc16(2) 保留(4)
 *
 * 写入分三步，提交点是 magic：
 *   1) 写负载
 *   2) 写页头的 seq/len/crc（magic 位置保持 0）
 *   3) 最后写 magic
 *
 * 第 2 步之后掉电，页头是「旧 magic + 新 CRC」，校验必然不过，
 * 于是回落到另一页；第 3 步之后掉电，新页已经完整，照常生效。
 * 换句话说，任何时刻都至少有一页可读。
 */
#include "mg_param.h"

#include <string.h>

#include "mg_crc16.h"

#define PARAM_VERSION 1u

static uint32_t page_off(int page)
{
    return (uint32_t)page * MG_PARAM_PAGE_SIZE;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

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

/* 有界版 strlen：参数区里的字符串可能没有结尾，不能直接 strlen */
static uint8_t str_len(const char *s, uint8_t max)
{
    uint8_t n = 0;

    while (n < max && s[n] != '\0')
        n++;
    return n;
}

/* ---------- 编解码 ---------- */

static void enc_str(uint8_t **o, const char *s, uint8_t max)
{
    uint8_t n = str_len(s, max);

    *(*o)++ = n;
    memcpy(*o, s, n);
    *o += n;
}

static int dec_str(const uint8_t **p, uint16_t *left, char *out, uint8_t max)
{
    uint8_t n;

    if (*left < 1)
        return -1;

    n = **p;
    (*p)++;
    (*left)--;

    /* n >= max 说明存进来的长度超出字段容量，或者压根不是本格式 */
    if (*left < n || n >= max)
        return -1;

    memcpy(out, *p, n);
    out[n] = '\0';
    *p += n;
    *left -= n;
    return 0;
}

int mg_param_encode(const mg_param_t *p, uint8_t *buf, uint16_t size, uint16_t *out_len)
{
    uint8_t tmp[MG_PARAM_PAYLOAD_MAX];
    uint8_t *o = tmp;

    if (p == 0 || buf == 0 || out_len == 0)
        return -1;
    if (p->ch_count > MG_PARAM_MAX_CH || p->poll_count > MG_PARAM_MAX_POLL)
        return -1;

    *o++ = PARAM_VERSION;
    *o++ = p->ch_count;
    *o++ = p->poll_count;
    *o++ = 0; /* 保留 */
    {
        uint8_t t[2];
        put_u16(t, p->poll_interval_ms);
        memcpy(o, t, 2);
        o += 2;
        put_u16(t, p->session_timeout_ms);
        memcpy(o, t, 2);
        o += 2;
    }

    for (uint8_t i = 0; i < p->ch_count; i++)
    {
        *o++ = p->ch[i].kind;
        enc_str(&o, p->ch[i].name, MG_PARAM_NAME_MAX);
        enc_str(&o, p->ch[i].arg, MG_PARAM_ARG_MAX);
    }

    for (uint8_t i = 0; i < p->poll_count; i++)
    {
        *o++ = p->poll[i].ch;
        *o++ = p->poll[i].slave;
        *o++ = p->poll[i].func;
        put_u16(o, p->poll[i].reg);
        o += 2;
        put_u16(o, p->poll[i].count);
        o += 2;
    }

    enc_str(&o, p->mqtt_addr, MG_PARAM_MQTT_MAX);
    enc_str(&o, p->mqtt_pub, MG_PARAM_MQTT_MAX);
    enc_str(&o, p->mqtt_sub, MG_PARAM_MQTT_MAX);

    *out_len = (uint16_t)(o - tmp);
    if (*out_len > size)
        return -1;

    memcpy(buf, tmp, *out_len);
    return 0;
}

int mg_param_decode(mg_param_t *p, const uint8_t *buf, uint16_t len)
{
    const uint8_t *q = buf;
    uint16_t left = len;

    if (p == 0 || buf == 0 || len < 8)
        return -1;

    memset(p, 0, sizeof(*p));

    if (*q++ != PARAM_VERSION)
        return -1;
    left--;

    p->ch_count = *q++;
    p->poll_count = *q++;
    q++; /* 保留 */
    left = (uint16_t)(left - 3);
    if (left < 4)
        return -1;

    if (p->ch_count > MG_PARAM_MAX_CH || p->poll_count > MG_PARAM_MAX_POLL)
        return -1;

    p->poll_interval_ms = get_u16(q);
    q += 2;
    p->session_timeout_ms = get_u16(q);
    q += 2;
    left -= 4;

    for (uint8_t i = 0; i < p->ch_count; i++)
    {
        if (left < 1)
            return -1;
        p->ch[i].kind = *q++;
        left--;
        if (dec_str(&q, &left, p->ch[i].name, MG_PARAM_NAME_MAX) != 0)
            return -1;
        if (dec_str(&q, &left, p->ch[i].arg, MG_PARAM_ARG_MAX) != 0)
            return -1;
    }

    for (uint8_t i = 0; i < p->poll_count; i++)
    {
        if (left < 7)
            return -1;
        p->poll[i].ch = *q++;
        p->poll[i].slave = *q++;
        p->poll[i].func = *q++;
        p->poll[i].reg = get_u16(q);
        q += 2;
        p->poll[i].count = get_u16(q);
        q += 2;
        left -= 7;
    }

    if (dec_str(&q, &left, p->mqtt_addr, MG_PARAM_MQTT_MAX) != 0)
        return -1;
    if (dec_str(&q, &left, p->mqtt_pub, MG_PARAM_MQTT_MAX) != 0)
        return -1;
    if (dec_str(&q, &left, p->mqtt_sub, MG_PARAM_MQTT_MAX) != 0)
        return -1;

    /*
     * 剩下的字节必须是 0。多出来的非零字节说明写入方比读取方新，
     * 这种「字段少了」的情况必须拒绝，否则会拿半份配置去跑。
     */
    while (left > 0)
    {
        if (*q++ != 0)
            return -1;
        left--;
    }

    return 0;
}

/* ---------- 校验与默认值 ---------- */

int mg_param_validate(const mg_param_t *p)
{
    if (p == 0)
        return -1;
    if (p->ch_count == 0 || p->ch_count > MG_PARAM_MAX_CH)
        return -1;
    if (p->poll_count == 0 || p->poll_count > MG_PARAM_MAX_POLL)
        return -1;
    if (p->poll_interval_ms < 10 || p->poll_interval_ms > 60000)
        return -1;
    if (p->session_timeout_ms < 10 || p->session_timeout_ms > 60000)
        return -1;

    for (uint8_t i = 0; i < p->ch_count; i++)
    {
        if (p->ch[i].name[0] == '\0')
            return -1;
        if (p->ch[i].kind != MG_KIND_CAN && p->ch[i].kind != MG_KIND_MODBUS)
            return -1;
    }

    for (uint8_t i = 0; i < p->poll_count; i++)
    {
        const mg_param_poll_t *pt = &p->poll[i];

        if (pt->ch >= p->ch_count)
            return -1;
        /* Modbus 从机地址 1..247，0 是广播、248 以上保留 */
        if (pt->slave == 0 || pt->slave > 247)
            return -1;
        if (pt->func < 1 || pt->func > 4)
            return -1;
        if (pt->count == 0 || pt->count > 16)
            return -1;
        /* 寄存器地址 + 个数不能越过 16 位空间 */
        if ((uint32_t)pt->reg + pt->count > 0x10000u)
            return -1;
    }

    return 0;
}

void mg_param_defaults(mg_param_t *p)
{
    if (p == 0)
        return;

    memset(p, 0, sizeof(*p));

    p->ch_count = 1;
    strncpy(p->ch[0].name, "modbus0", MG_PARAM_NAME_MAX - 1);
    p->ch[0].kind = MG_KIND_MODBUS;
    strncpy(p->ch[0].arg, "loop", MG_PARAM_ARG_MAX - 1);

    /* 两个从机 × 四个保持寄存器，和演示程序的从机模型一致 */
    p->poll_count = 8;
    for (uint8_t s = 0; s < 2; s++)
    {
        for (uint8_t r = 0; r < 4; r++)
        {
            mg_param_poll_t *pt = &p->poll[s * 4 + r];

            pt->ch = 0;
            pt->slave = (uint8_t)(s + 1);
            pt->func = 3; /* 读保持寄存器 */
            pt->reg = r;
            pt->count = 1;
        }
    }

    p->poll_interval_ms = 100;
    p->session_timeout_ms = 500;

    strncpy(p->mqtt_addr, "tcp://127.0.0.1:1883", MG_PARAM_MQTT_MAX - 1);
    strncpy(p->mqtt_pub, "gw/uplink", MG_PARAM_MQTT_MAX - 1);
    strncpy(p->mqtt_sub, "gw/downlink", MG_PARAM_MQTT_MAX - 1);
}

/* ---------- 双页存储 ---------- */

typedef struct
{
    int valid;
    uint32_t seq;
    uint16_t len;
} page_meta_t;

static int page_read(const mg_storage_io_t *io, int page, page_meta_t *meta,
                     uint8_t *payload)
{
    uint8_t hdr[MG_PARAM_HDR_SIZE];
    uint16_t len, want, crc;
    uint32_t off = page_off(page);

    meta->valid = 0;
    meta->seq = 0;
    meta->len = 0;

    if (io->read(io->ctx, off, hdr, sizeof(hdr)) != 0)
        return -1;

    /* magic 是提交标记，没写全的页直接跳过 */
    if (hdr[0] != 'M' || hdr[1] != 'G' || hdr[2] != 'P' || hdr[3] != '1')
        return 0;

    meta->seq = get_u32(&hdr[4]);
    len = get_u16(&hdr[8]);
    want = get_u16(&hdr[10]);

    if (len == 0 || len > MG_PARAM_PAYLOAD_MAX)
        return 0;

    if (io->read(io->ctx, off + MG_PARAM_HDR_SIZE, payload, len) != 0)
        return -1;

    {
        /* CRC 覆盖「序号 + 长度 + 负载」，防止旧页头配新负载这类错配 */
        uint8_t head[6];
        uint16_t crc_v;

        put_u32(head, meta->seq);
        put_u16(&head[4], len);
        crc_v = mg_crc16(head, sizeof(head));
        for (uint16_t i = 0; i < len; i++)
            crc_v = mg_crc16_update(crc_v, payload[i]);
        crc = crc_v;
    }

    if (crc != want)
        return 0;

    meta->valid = 1;
    meta->len = len;
    return 0;
}

static int write_page(const mg_storage_io_t *io, int page, uint32_t seq,
                      const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[MG_PARAM_HDR_SIZE];
    uint8_t magic[4];
    uint8_t head[6];
    uint32_t off = page_off(page);
    uint16_t crc;

    put_u32(head, seq);
    put_u16(&head[4], len);
    crc = mg_crc16(head, sizeof(head));
    for (uint16_t i = 0; i < len; i++)
        crc = mg_crc16_update(crc, payload[i]);

    /*
     * 页头先算好，但 magic 留空：magic 是最后写的提交标记。
     * 顺序：负载 -> 页头（magic 为 0）-> magic。
     */
    memset(hdr, 0, sizeof(hdr));
    put_u32(&hdr[4], seq);
    put_u16(&hdr[8], len);
    put_u16(&hdr[10], crc);

    if (io->write(io->ctx, off + MG_PARAM_HDR_SIZE, payload, len) != 0)
        return -1;

    /* 先让这一页失效，再写页头，避免旧 magic 与新 CRC 短暂共存被误判 */
    memset(magic, 0, sizeof(magic));
    if (io->write(io->ctx, off, magic, sizeof(magic)) != 0)
        return -1;

    if (io->write(io->ctx, off + 4, hdr + 4, MG_PARAM_HDR_SIZE - 4) != 0)
        return -1;

    magic[0] = 'M';
    magic[1] = 'G';
    magic[2] = 'P';
    magic[3] = '1';
    if (io->write(io->ctx, off, magic, sizeof(magic)) != 0)
        return -1;

    return 0;
}

static int scan(const mg_storage_io_t *io, page_meta_t *m, uint8_t *payload,
                int *page_out)
{
    /*
     * 两页必须各用一块缓冲。
     * 共用一块的话，读完第二页之后缓冲里是第二页的内容，
     * 而胜出的可能是第一页 —— 结果就是「读到了有效页头，却拿着另一页的负载」，
     * 而且 CRC 校验的是页内自己那份，不会报错。
     */
    static uint8_t buf0[MG_PARAM_PAYLOAD_MAX];
    static uint8_t buf1[MG_PARAM_PAYLOAD_MAX];
    page_meta_t m0, m1;

    if (page_read(io, 0, &m0, buf0) != 0)
        return -1;
    if (page_read(io, 1, &m1, buf1) != 0)
        return -1;

    /*
     * 记录胜出的页号。只看 load 的返回值不够 —— 调用方要写日志
     * 「这次参数是从哪一页来的」，靠序号奇偶去猜是错的。
     */
    if (page_out != 0)
        *page_out = -1;

    memset(m, 0, sizeof(*m));

    /*
     * 取序号大的那页。用有符号差比较，这样 2^32 回绕时仍然正确 ——
     * 直接用 `>` 比大小的话，序号一旦回绕就会永远选中旧页。
     */
    if (m0.valid && m1.valid)
    {
        int one = ((int32_t)(m1.seq - m0.seq) > 0);
        *m = one ? m1 : m0;
        if (page_out != 0)
            *page_out = one;
        memcpy(payload, one ? buf1 : buf0, m->len);
    }
    else if (m0.valid)
    {
        *m = m0;
        if (page_out != 0)
            *page_out = 0;
        memcpy(payload, buf0, m->len);
    }
    else if (m1.valid)
    {
        *m = m1;
        if (page_out != 0)
            *page_out = 1;
        memcpy(payload, buf1, m->len);
    }

    return 0;
}

int mg_param_load(mg_param_t *out, const mg_storage_io_t *io,
                  const mg_param_t *def, int *from_page)
{
    page_meta_t m;
    uint8_t payload[MG_PARAM_PAYLOAD_MAX];
    int page = -1;

    if (out == 0 || io == 0 || io->read == 0)
        return -1;
    if (from_page != 0)
        *from_page = -1;

    if (scan(io, &m, payload, &page) != 0)
        return -1;

    if (m.valid)
    {
        if (mg_param_decode(out, payload, m.len) == 0 && mg_param_validate(out) == 0)
        {
            if (from_page != 0)
                *from_page = page;
            return 0;
        }

        /*
         * 页是完整的（CRC 通过）但内容不合法：多半是固件升级后字段语义变了。
         * 这种情况按「没有参数」处理并用默认值，而不是带着半份配置继续跑。
         */
    }

    if (def != 0)
        *out = *def;
    else
        mg_param_defaults(out);

    return 1;
}

int mg_param_save(const mg_param_t *p, const mg_storage_io_t *io)
{
    page_meta_t active;
    uint8_t probe[MG_PARAM_PAYLOAD_MAX];
    uint8_t payload[MG_PARAM_PAYLOAD_MAX];
    uint8_t verify[MG_PARAM_PAYLOAD_MAX];
    page_meta_t check;
    uint16_t len = 0;
    int target, active_page;
    uint32_t seq;

    if (p == 0 || io == 0 || io->write == 0)
        return -1;
    if (mg_param_validate(p) != 0)
        return -1;
    if (mg_param_encode(p, payload, sizeof(payload), &len) != 0)
        return -1;

    if (scan(io, &active, probe, 0) != 0)
        return -1;

    if (active.valid)
    {
        /* 找到当前有效页：两页都读一遍，序号与 active 相同且有效的那一页 */
        page_meta_t m0, m1;
        uint8_t tmp[MG_PARAM_PAYLOAD_MAX];

        if (page_read(io, 0, &m0, tmp) != 0 || page_read(io, 1, &m1, tmp) != 0)
            return -1;
        active_page = (m0.valid && m0.seq == active.seq) ? 0 : 1;
        seq = active.seq + 1;
    }
    else
    {
        /* 两页都空：第一次写到页 1，页 0 留给下一次 —— 交替从哪一页起步都行，
         * 固定下来是为了让日志与测试有确定的预期 */
        active_page = 0;
        seq = 1;
    }

    target = active_page ^ 1;

    if (write_page(io, target, seq, payload, len) != 0)
        return -1;

    /* 回读校验：写的时候报成功、读回来不对（Flash 位翻转、扇区坏）也能发现 */
    if (page_read(io, target, &check, verify) != 0)
        return -1;
    if (!check.valid || check.seq != seq || check.len != len ||
        memcmp(verify, payload, len) != 0)
        return -1;

    return 0;
}

int mg_param_active_page(const mg_storage_io_t *io)
{
    page_meta_t m0, m1;
    uint8_t tmp[MG_PARAM_PAYLOAD_MAX];

    if (io == 0 || io->read == 0)
        return -1;

    if (page_read(io, 0, &m0, tmp) != 0 || page_read(io, 1, &m1, tmp) != 0)
        return -1;

    if (m0.valid && m1.valid)
        return ((int32_t)(m1.seq - m0.seq) > 0) ? 1 : 0;
    if (m0.valid)
        return 0;
    if (m1.valid)
        return 1;
    return -1;
}
