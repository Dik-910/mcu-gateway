/*
 * mg_app.h —— 网关任务组装
 *
 * 任务划分（这是本项目最能讲的部分）：
 *
 *   rx_task[每通道]  收帧入队。只做拷贝，不做解析 ——
 *                    解析一旦放到接收任务里，慢通道会拖住快通道。
 *   parse_task       出队 -> 校验 -> 会话匹配 -> 组 JSON -> 上报。
 *                    单实例，保证上报顺序与接收顺序一致。
 *   poll_task        周期下发 Modbus 读请求、收割超时会话。
 *                    与解析分离，因为「发」和「收」的节奏不同。
 *   monitor_task     周期打印统计与各任务栈水位。
 *
 * 为什么不学课程项目那样「一个通道一个完整处理任务」：
 * 那样每个通道都要各写一份解析 + 上报逻辑，加第三路通道时复制粘贴，
 * 三份代码迟早不一致。这里把「收」按通道拆、把「处理」收成一份。
 *
 * 代价是 parse_task 成了单点：如果它有阻塞操作（比如同步发 MQTT），
 * 整条链路都会卡住。所以 uplink 必须是短操作，超时要设在上层。
 */
#ifndef MG_APP_H
#define MG_APP_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "mg_channel.h"
#include "mg_session.h"
#include "mg_uplink.h"

#define MG_RX_QUEUE_DEPTH     32      /* 收帧队列深度：够吸收一次突发 */
#define MG_POLL_INTERVAL_MS   100     /* 轮询间隔，决定最坏响应延迟 */
#define MG_MONITOR_PERIOD_MS  5000    /* 统计打印周期 */

typedef struct
{
    mg_channel_t ch[MG_CHANNEL_MAX];
    uint8_t ch_count;

    QueueHandle_t rx_q;
    mg_sess_tbl_t sess;
    const mg_uplink_ops_t *uplink;

    TaskHandle_t rx_task[MG_CHANNEL_MAX];
    TaskHandle_t parse_task;
    TaskHandle_t poll_task;
    TaskHandle_t monitor_task;

    volatile int running;

    /* 统计 */
    uint32_t rx_dropped;     /* 队列满丢帧 —— 这个数字持续增长说明该加深队列或降频 */
    uint32_t parse_errors;   /* CRC 错、长度错 */
    uint32_t json_errors;
    uint32_t uplink_errors;
} mg_gw_t;

int  mg_gw_init(mg_gw_t *gw, const mg_uplink_ops_t *up);

int  mg_gw_add_channel(mg_gw_t *gw, const char *name, mg_chan_kind_t kind,
                       const char *arg, uint8_t *out_index);

/* 创建所有任务。调用前调度器尚未启动，或已启动均可 */
int  mg_gw_start(mg_gw_t *gw);

/* 请求停止：置标志并等任务退出。RTOS 上一般不需要，测试里需要 */
void mg_gw_stop(mg_gw_t *gw);

void mg_gw_print_stats(mg_gw_t *gw);

#endif /* MG_APP_H */
