/*
 * mg_uplink_stdout.c —— 打到标准输出的上行实现
 *
 * 演示和测试用。真正的价值是：让整条链路（收帧 -> 会话匹配 -> JSON）
 * 在没有 broker、没有硬件的情况下就能验证。
 */
#include "mg_uplink.h"
#include <stdio.h>

static uint32_t g_count;

static int up_open(void)
{
    printf("[uplink:stdout] ready\n");
    return 0;
}

static int up_publish(const char *json)
{
    if (json == 0)
        return -1;

    g_count++;
    printf("[uplink:stdout] #%u %s\n", (unsigned)g_count, json);
    fflush(stdout);
    return 0;
}

static void up_close(void)
{
    printf("[uplink:stdout] closed, total %u\n", (unsigned)g_count);
}

static const mg_uplink_ops_t g_stdout_ops =
{
    up_open, up_publish, up_close
};

const mg_uplink_ops_t *mg_uplink_stdout_ops(void)
{
    return &g_stdout_ops;
}
