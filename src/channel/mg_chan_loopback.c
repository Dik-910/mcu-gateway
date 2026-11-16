/*
 * mg_chan_loopback.c —— 内存回环通道（模拟一个对端设备）
 *
 * 存在的意义：让上层任务在完全没有硬件、也没有 vcan 的情况下就能跑通。
 *
 * 它模拟的是一根双向总线加一个对端设备，所以有两个方向：
 *
 *   网关 send  -->  to_peer  -->  测试代码 take    （对端收到请求）
 *   网关 recv  <--  to_gw    <--  测试代码 inject  （对端发回响应）
 *
 * 不是「发出去自己立刻收到」：那会让「请求」和「响应」混为一谈，
 * 测不出会话匹配是否真的在按 (slave, func) 找对象。
 *
 * 另外保留两条真实设备特性：
 *   1. 队列固定深度，满了丢并计数 —— 真实串口/CAN 的接收缓冲也会满
 *   2. 非阻塞逐帧取出 —— 调用方必须自己处理「暂时没有数据」
 */
#include "mg_channel.h"
#include <string.h>

#define LOOP_DEPTH 16

typedef struct
{
    mg_frame_t to_peer[LOOP_DEPTH];   /* 网关 -> 对端 */
    volatile uint32_t tp_head, tp_tail;
    mg_frame_t to_gw[LOOP_DEPTH];     /* 对端 -> 网关 */
    volatile uint32_t tg_head, tg_tail;
    volatile uint32_t dropped;
} loop_ctx_t;

static loop_ctx_t g_loop[MG_CHANNEL_MAX];

static int loop_open(mg_channel_t *ch, const char *arg)
{
    (void)arg;

    if (ch->index >= MG_CHANNEL_MAX)
        return -1;

    memset(&g_loop[ch->index], 0, sizeof(loop_ctx_t));
    ch->ctx = &g_loop[ch->index];
    return 0;
}

static int loop_recv(mg_channel_t *ch, mg_frame_t *f)
{
    loop_ctx_t *c = (loop_ctx_t *)ch->ctx;

    if (c == 0)
        return MG_ERR_STATE;

    if (c->tg_tail == c->tg_head)
        return MG_ERR_NOMSG;

    *f = c->to_gw[c->tg_tail % LOOP_DEPTH];
    c->tg_tail++;
    f->channel = ch->index;
    return MG_OK;
}

static int loop_send(mg_channel_t *ch, const mg_frame_t *f)
{
    loop_ctx_t *c = (loop_ctx_t *)ch->ctx;

    if (c == 0)
        return -1;

    if (c->tp_head - c->tp_tail >= LOOP_DEPTH)
    {
        c->dropped++;
        return -1;
    }

    c->to_peer[c->tp_head % LOOP_DEPTH] = *f;
    c->tp_head++;
    return 0;
}

static void loop_close(mg_channel_t *ch)
{
    ch->ctx = 0;
}

static const mg_channel_ops_t g_loop_ops =
{
    loop_open, loop_recv, loop_send, loop_close
};

const mg_channel_ops_t *mg_chan_loopback_ops(void)
{
    return &g_loop_ops;
}

int mg_chan_loopback_inject(mg_channel_t *ch, const mg_frame_t *f)
{
    loop_ctx_t *c;

    if (ch == 0 || f == 0 || ch->ctx == 0)
        return MG_ERR_PARAM;

    c = (loop_ctx_t *)ch->ctx;

    if (c->tg_head - c->tg_tail >= LOOP_DEPTH)
    {
        c->dropped++;
        return MG_ERR_FULL;
    }

    c->to_gw[c->tg_head % LOOP_DEPTH] = *f;
    c->tg_head++;
    return MG_OK;
}

int mg_chan_loopback_take(mg_channel_t *ch, mg_frame_t *f)
{
    loop_ctx_t *c;

    if (ch == 0 || f == 0 || ch->ctx == 0)
        return MG_ERR_PARAM;

    c = (loop_ctx_t *)ch->ctx;

    if (c->tp_tail == c->tp_head)
        return MG_ERR_NOMSG;

    *f = c->to_peer[c->tp_tail % LOOP_DEPTH];
    c->tp_tail++;
    return MG_OK;
}
