/*
 * mg_channel_test.c —— 通道抽象与回环实现
 *
 * 重点验证两个方向互不串味：
 *   网关 send -> 对端 take    （对端收到请求）
 *   网关 recv <- 对端 inject  （网关收到响应）
 *
 * 如果桩把「发出去」也塞进接收队列，请求就会被当成响应解析，
 * 会话匹配立刻错乱 —— 而单靠肉眼看代码很难发现。
 */
#include <stdio.h>
#include <string.h>
#include "mg_channel.h"
#include "log.h"

static int g_fail;
static int g_total;

static void check(int cond, const char *what)
{
    g_total++;
    if (!cond)
    {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void mk(mg_frame_t *f, uint32_t id, uint8_t dlc, uint8_t first)
{
    memset(f, 0, sizeof(*f));
    f->id = id;
    f->dlc = dlc;
    for (uint8_t i = 0; i < dlc && i < MG_FRAME_DATA; i++)
        f->data[i] = (uint8_t)(first + i);
}

static void test_basic(void)
{
    mg_channel_t ch;
    mg_frame_t f, out;

    printf("\nOpen / recv empty\n");

    ch.index = 0;
    check(mg_channel_open(&ch, "loop0", MG_KIND_MODBUS, mg_chan_loopback_ops(), "x") == MG_OK,
          "open ok");

    check(mg_channel_recv(&ch, &out) == MG_ERR_NOMSG, "empty channel returns NOMSG");
    check(ch.rx_errors == 0, "NOMSG is not counted as error");

    /* 对端发一帧，网关收到 */
    mk(&f, 0x123, 7, 0x10);
    check(mg_chan_loopback_inject(&ch, &f) == MG_OK, "inject ok");
    check(mg_channel_recv(&ch, &out) == MG_OK, "recv after inject");
    check(out.id == 0x123 && out.dlc == 7 && out.data[0] == 0x10, "payload intact");
    check(out.channel == 0, "channel index stamped");
    check(ch.rx_frames == 1, "rx counter");

    check(mg_channel_recv(&ch, &out) == MG_ERR_NOMSG, "drained");

    mg_channel_close(&ch);
}

static void test_direction_isolation(void)
{
    mg_channel_t ch;
    mg_frame_t f, out;

    printf("\nTwo directions are isolated\n");

    ch.index = 1;
    check(mg_channel_open(&ch, "loop1", MG_KIND_MODBUS, mg_chan_loopback_ops(), "x") == MG_OK,
          "open ok");

    /* 网关发出去：不应该出现在自己的接收队列里 */
    mk(&f, 0x7E0, 8, 0xAA);
    check(mg_channel_send(&ch, &f) == MG_OK, "send ok");
    check(mg_channel_recv(&ch, &out) == MG_ERR_NOMSG,
          "own transmission does NOT come back as received");

    /* 对端能取到这帧 */
    check(mg_chan_loopback_take(&ch, &out) == MG_OK, "peer takes the request");
    check(out.id == 0x7E0 && out.data[0] == 0xAA, "peer sees the same payload");
    check(mg_chan_loopback_take(&ch, &out) == MG_ERR_NOMSG, "peer queue drained");
    check(ch.tx_frames == 1, "tx counter");

    mg_channel_close(&ch);
}

static void test_validation(void)
{
    mg_channel_t ch;
    mg_frame_t f;

    printf("\nValidation\n");

    ch.index = 2;
    check(mg_channel_open(0, "x", MG_KIND_CAN, mg_chan_loopback_ops(), "x") == MG_ERR_PARAM,
          "null channel rejected");
    check(mg_channel_open(&ch, "loop2", MG_KIND_CAN, 0, "x") == MG_ERR_PARAM,
          "null ops rejected");
    check(mg_channel_open(&ch, "loop2", MG_KIND_CAN, mg_chan_loopback_ops(), 0) == MG_ERR_PARAM,
          "null arg rejected");

    check(mg_channel_open(&ch, "loop2", MG_KIND_CAN, mg_chan_loopback_ops(), "x") == MG_OK,
          "open for validation ok");

    /* dlc 越界必须在包装层被拦掉 */
    mk(&f, 0x100, 9, 0);
    check(mg_channel_send(&ch, &f) == MG_ERR_PARAM, "dlc > 8 rejected on send");
    check(ch.tx_errors == 0, "rejected send does not bump tx_errors");

    check(mg_channel_send(&ch, 0) == MG_ERR_PARAM, "null frame rejected");

    mg_channel_close(&ch);

    check(mg_channel_recv(0, &f) == MG_ERR_PARAM, "recv after close is param error");
}

static void test_queue_full(void)
{
    mg_channel_t ch;
    mg_frame_t f, out;
    int accepted = 0, rejected = 0;

    printf("\nQueue depth is finite\n");

    ch.index = 3;
    mg_channel_open(&ch, "loop3", MG_KIND_MODBUS, mg_chan_loopback_ops(), "x");

    mk(&f, 0x200, 8, 0x01);

    /* 一直灌，直到被拒绝：真实的接收缓冲也会满 */
    for (int i = 0; i < 64; i++)
    {
        if (mg_chan_loopback_inject(&ch, &f) == MG_OK)
            accepted++;
        else
            rejected++;
    }

    check(accepted == 16, "accepted exactly queue depth");
    check(rejected > 0, "overflow rejected instead of silently growing");
    check(ch.rx_errors == 0, "inject overflow is not an rx error");

    /* 取空后仍能继续工作 */
    while (mg_channel_recv(&ch, &out) == MG_OK)
        ;

    check(ch.rx_frames == 16, "all accepted frames received");
    check(mg_chan_loopback_inject(&ch, &f) == MG_OK, "usable after drain");

    mg_channel_close(&ch);
}

int main(void)
{
    log_set_level(LOG_ERROR);   /* 测试里不要 INFO 噪音 */

    printf("=== mg_channel_test ===\n");

    test_basic();
    test_direction_isolation();
    test_validation();
    test_queue_full();

    printf("\nResult: %d checks, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
