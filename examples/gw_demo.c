/*
 * gw_demo.c —— 无硬件演示
 *
 * 用回环通道模拟一个 Modbus 从机，把整条链路跑起来：
 *
 *   poll_task 发读请求 -> 通道 to_peer -> slave_task 取走
 *   slave_task 组响应 -> 通道 to_gw  -> rx_task -> rx_q
 *   parse_task 校验 + 会话匹配 -> JSON -> uplink(stdout)
 *
 * 这个演示证明的是：任务划分、队列、会话超时、CRC 校验、JSON 组包
 * 全都能在没有板子、没有 vcan 的情况下验证。
 *
 * 用法：./gw-demo [--seconds N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "mg_app.h"
#include "mg_modbus.h"
#include "mg_param_file.h"
#include "log.h"

static int g_seconds = 8;

/*
 * 模拟从机：收到读请求，回一个寄存器值。
 *
 * 故意把响应做成 7 字节而不是 8 字节 ——
 * 真实的 Modbus 响应长度不固定，如果解析层写死了 8 字节，
 * 这个用例就会失败。用真实长度更接近现场。
 */
static void slave_task(void *p)
{
    mg_gw_t *gw = (mg_gw_t *)p;
    mg_frame_t req;

    printf("[slave] Modbus 从机已就绪（地址 1..2，寄存器 0..3）\n");

    for (;;)
    {
        if (mg_chan_loopback_take(&gw->ch[0], &req) == MG_OK)
        {
            uint8_t resp[7];
            uint16_t reg, val, crc;
            mg_frame_t rsp;

            /* 请求帧布局：slave, func, reg_hi, reg_lo, cnt_hi, cnt_lo, crc */
            reg = (uint16_t)((req.data[2] << 8) | req.data[3]);

            /* 造一个可预测的值：寄存器号 * 100，方便肉眼核对 */
            val = (uint16_t)(reg * 100);

            resp[0] = req.data[0];
            resp[1] = req.data[1];
            resp[2] = 0x02;
            resp[3] = (uint8_t)(val >> 8);
            resp[4] = (uint8_t)(val & 0xFF);

            crc = mg_mb_crc16(resp, 5);
            resp[5] = (uint8_t)(crc & 0xFF);
            resp[6] = (uint8_t)(crc >> 8);

            memset(&rsp, 0, sizeof(rsp));
            rsp.id = req.id;
            rsp.dlc = sizeof(resp);
            memcpy(rsp.data, resp, sizeof(resp));

            mg_chan_loopback_inject(&gw->ch[0], &rsp);
        }
        else
        {
            /* 没有请求时让出 CPU，否则会把低优先级任务饿死 */
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/* 到点收工。真实固件没有这个任务 —— 这里只是为了演示能自己结束 */
static void shutdown_task(void *p)
{
    mg_gw_t *gw = (mg_gw_t *)p;

    vTaskDelay(pdMS_TO_TICKS((uint32_t)g_seconds * 1000));

    printf("\n[main] %d 秒到，停止网关\n", g_seconds);
    mg_gw_print_stats(gw);

    mg_gw_stop(gw);
    vTaskEndScheduler();

    vTaskDelete(NULL);
}

int main(int argc, char **argv)
{
    static mg_gw_t gw;

    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            g_seconds = atoi(argv[++i]);

    log_set_level(LOG_INFO);

    printf("=== mcu-gateway 演示（回环通道，无需硬件）===\n");

    if (mg_gw_init(&gw, mg_uplink_stdout_ops()) != MG_OK)
    {
        printf("init failed\n");
        return 1;
    }

    /*
     * 通道表与轮询表来自参数区，不再写死在代码里。
     * 首次启动没有有效参数时写入默认值；之后改参数不必重新编译。
     */
    {
        mg_param_t prm;
        const char *ppath = getenv("MG_PARAM_FILE");
        int page = -1;

        if (mg_param_load_file(ppath ? ppath : "gw_param.bin", &prm, &page) != MG_OK)
        {
            printf("param load failed\n");
            return 1;
        }

        if (mg_gw_apply_param(&gw, &prm) != MG_OK)
        {
            printf("apply param failed\n");
            return 1;
        }

        log_info("param: %d channel(s), %d poll entries, interval %d ms, source=%s",
                 prm.ch_count, prm.poll_count, prm.poll_interval_ms,
                 page < 0 ? "defaults" : (page == 0 ? "page0" : "page1"));
    }

    if (mg_gw_start(&gw) != MG_OK)
    {
        printf("start failed\n");
        return 1;
    }

    xTaskCreate(slave_task, "slave", 1024, &gw, 2, NULL);
    xTaskCreate(shutdown_task, "shut", 1024, &gw, 1, NULL);

    vTaskStartScheduler();

    printf("=== 调度器已退出 ===\n");
    mg_channel_close(&gw.ch[0]);
    return 0;
}
