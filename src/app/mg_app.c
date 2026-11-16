/*
 * mg_app.c —— 网关任务实现
 *
 * 数据流：
 *   硬件/回环 --rx_task--> rx_q --parse_task--> 校验 -> 会话匹配 -> JSON -> uplink
 *                                        \
 *                                         +-> mg_sess_expire 收割超时
 *   poll_task --(周期)--> 下行请求 --> 会话表占位
 */
#include "mg_app.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "log.h"
#include "mg_modbus.h"

/* ---------- 时间基准 ---------- */

/*
 * tick 换算成毫秒。configTICK_RATE_HZ = 1000 时 1 tick = 1 ms，
 * 但仍然走 portTICK_PERIOD_MS，将来改 tick 频率不用改这里。
 */
static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* xTaskCreate 只能传一个参数，用静态结构把 (gw, 通道号) 带进去 */
typedef struct
{
    mg_gw_t *gw;
    uint8_t idx;
} rx_arg_t;

static rx_arg_t g_rx_arg[MG_CHANNEL_MAX];

/* ---------- 上行 ---------- */

/*
 * 组一帧上报 JSON。
 *
 * 与课程项目的区别：
 *   1. 每次上报都新建对象、用完立即释放，不用 static 累积
 *   2. 带上 sid（会话号），出问题时能顺着日志找回是哪次请求
 *   3. 异常响应单独标记，不和正常数据混在一起
 */
static void report(mg_gw_t *gw, const mg_frame_t *f, uint16_t sid,
                   uint16_t value, uint8_t exc)
{
    cJSON *o = cJSON_CreateObject();
    char *s;

    if (o == 0)
    {
        gw->json_errors++;
        return;
    }

    cJSON_AddStringToObject(o, "gw", "mcu-gateway");
    cJSON_AddStringToObject(o, "ch", gw->ch[f->channel].name);
    cJSON_AddStringToObject(o, "type",
                            gw->ch[f->channel].kind == MG_KIND_CAN ? "can" : "modbus");
    cJSON_AddNumberToObject(o, "slave", f->data[0]);
    cJSON_AddNumberToObject(o, "func", f->data[1] & 0x7F);
    cJSON_AddNumberToObject(o, "sid", sid);      /* 0 表示没匹配上会话 */
    cJSON_AddNumberToObject(o, "ts", f->ts_ms);

    /*
     * 寄存器地址必须从会话里取：Modbus 的读响应只带数据，不带地址。
     * 没有这个字段，上报出去的数据就是「某个 slave 的某个值是 200」，
     * 上游无法知道是哪个点 —— 也就没法验证下行写入是否生效。
     */
    if (sid != 0)
    {
        mg_session_t *sess = mg_sess_get(&gw->sess, sid);
        if (sess != 0)
            cJSON_AddNumberToObject(o, "reg", sess->reg);
    }

    if (exc != 0)
    {
        /* 异常响应：带上异常码，不要伪装成数值 */
        cJSON_AddNumberToObject(o, "exc", exc);
    }
    else
    {
        cJSON_AddNumberToObject(o, "value", value);
    }

    s = cJSON_PrintUnformatted(o);

    if (s == 0)
    {
        gw->json_errors++;
    }
    else
    {
        if (gw->uplink != 0 && gw->uplink->publish != 0)
        {
            if (gw->uplink->publish(s) != 0)
                gw->uplink_errors++;
        }
        cJSON_free(s);
    }

    cJSON_Delete(o);
}

/*
 * 写响应（功能码 0x06）单独处理。
 *
 * 它的格式和读响应不同：没有「字节数」字段，直接回显请求的
 * 寄存器地址和值。放进通用解析会把它当成读响应，
 * 解出来的 value 是错的（会取到 reg 和 value 的高字节拼起来）。
 */
static void report_write_ack(mg_gw_t *gw, const mg_frame_t *f)
{
    cJSON *o = cJSON_CreateObject();
    char *str;

    if (o == 0)
    {
        gw->json_errors++;
        return;
    }

    cJSON_AddStringToObject(o, "gw", "mcu-gateway");
    cJSON_AddStringToObject(o, "ch", gw->ch[f->channel].name);
    cJSON_AddStringToObject(o, "type", "write_ack");
    cJSON_AddNumberToObject(o, "slave", f->data[0]);
    cJSON_AddNumberToObject(o, "reg", (f->data[2] << 8) | f->data[3]);
    cJSON_AddNumberToObject(o, "value", (f->data[4] << 8) | f->data[5]);
    cJSON_AddNumberToObject(o, "ts", f->ts_ms);

    str = cJSON_PrintUnformatted(o);

    if (str == 0)
    {
        gw->json_errors++;
    }
    else
    {
        if (gw->uplink != 0 && gw->uplink->publish != 0)
        {
            if (gw->uplink->publish(str) != 0)
                gw->uplink_errors++;
        }
        cJSON_free(str);
    }

    cJSON_Delete(o);
}

/*
 * 超时回收回调。
 *
 * 超时必须上报，不能只记一条日志：上游需要知道「某个点读不到」，
 * 否则界面上这个点会一直显示上一次的值，看起来一切正常。
 */
static void on_session_timeout(const mg_session_t *s, void *user)
{
    mg_gw_t *gw = (mg_gw_t *)user;
    cJSON *o = cJSON_CreateObject();
    char *str;

    if (o == 0)
    {
        gw->json_errors++;
        return;
    }

    cJSON_AddStringToObject(o, "gw", "mcu-gateway");
    cJSON_AddStringToObject(o, "type", "timeout");
    cJSON_AddNumberToObject(o, "slave", s->slave);
    cJSON_AddNumberToObject(o, "func", s->func);
    cJSON_AddNumberToObject(o, "reg", s->reg);
    cJSON_AddNumberToObject(o, "sid", s->sid);
    cJSON_AddNumberToObject(o, "timeout_ms", s->timeout_ms);

    str = cJSON_PrintUnformatted(o);

    if (str == 0)
    {
        gw->json_errors++;
    }
    else
    {
        if (gw->uplink != 0 && gw->uplink->publish != 0)
        {
            if (gw->uplink->publish(str) != 0)
                gw->uplink_errors++;
        }
        cJSON_free(str);
    }

    cJSON_Delete(o);
}

/* ---------- 解析 ---------- */

static void handle_frame(mg_gw_t *gw, const mg_frame_t *f)
{
    uint16_t value = 0;
    uint8_t exc = 0;
    uint16_t sid;

    /* 最短合法 Modbus 响应：地址+功能码+字节数+数据+CRC = 5 字节 */
    if (f->dlc < 5)
    {
        gw->parse_errors++;
        return;
    }

    /* 写响应走单独分支：格式与读响应不同 */
    if ((f->data[1] & 0x7F) == MB_FUNC_WRITE_REGISTER)
    {
        uint16_t crc_calc, crc_recv;

        if (f->dlc < 7)
        {
            gw->parse_errors++;
            return;
        }

        crc_calc = mg_mb_crc16(f->data, (uint16_t)(f->dlc - 2));
        crc_recv = (uint16_t)(f->data[f->dlc - 2] | (f->data[f->dlc - 1] << 8));

        if (crc_calc != crc_recv)
        {
            gw->parse_errors++;
            return;
        }

        report_write_ack(gw, f);
        return;
    }

    /*
     * CRC 在这里校验，不通过直接丢。
     * 一个 CRC 错的寄存器值和「没读到」是两个概念 ——
     * 前者会让上层以为设置生效了，后者至少会触发重试。
     */
    if (mg_mb_parse_response(f->data, f->dlc, f->data[0], f->data[1] & 0x7F,
                             &value, &exc) != MG_OK)
    {
        gw->parse_errors++;
        return;
    }

    /* 先匹配会话：匹配上说明这是我们要的响应；匹配不上也要上报（主动上报型节点） */
    sid = mg_sess_on_response(&gw->sess, f->data[0], f->data[1] & 0x7F, value, exc);

    report(gw, f, sid, value, exc);

    /* 处理完立刻释放，避免槽位被 DONE 状态长期占用 */
    if (sid != 0)
        mg_sess_release(&gw->sess, sid);
}

/* ---------- 任务 ---------- */

static void rx_task(void *p)
{
    rx_arg_t *a = (rx_arg_t *)p;
    mg_gw_t *gw = a->gw;
    mg_frame_t f;

    log_info("rx_task[%u] started for %s", a->idx, gw->ch[a->idx].name);

    while (gw->running)
    {
        if (mg_channel_recv(&gw->ch[a->idx], &f) == MG_OK)
        {
            f.ts_ms = now_ms();

            /*
             * 非阻塞投递。队列满是设计内的状态：
             * 这里若用 portMAX_DELAY，某一通道队列堵塞会连带拖住本任务，
             * 而本任务还负责收帧 —— 等于把「处理慢」放大成「收不到」。
             * 宁可丢帧并计数，让统计暴露出来。
             */
            if (xQueueSend(gw->rx_q, &f, 0) != pdPASS)
                gw->rx_dropped++;
        }
        else
        {
            /*
             * 无数据时让出 CPU。1 ms 是 PC 上的折中：
             * 真实 STM32 应该改成中断 -> 信号量唤醒，延迟能从 1 ms 降到微秒级。
             * 这里保持轮询是为了让同一份任务代码也能跑在没有中断的回环通道上。
             */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    log_info("rx_task[%u] exit", a->idx);
    vTaskDelete(NULL);
}

static void parse_task(void *p)
{
    mg_gw_t *gw = (mg_gw_t *)p;
    mg_frame_t f;

    log_info("parse_task started");

    while (gw->running)
    {
        /* 50 ms 超时：只为让下面的 expire 有机会跑，不阻塞退出 */
        if (xQueueReceive(gw->rx_q, &f, pdMS_TO_TICKS(50)) == pdPASS)
            handle_frame(gw, &f);

        /*
         * 超时收割放在这里，而不是单独一个定时器任务：
         * 会话表只有解析任务会访问，放在同一任务里就不需要加锁。
         * 多任务访问同一张表就必须上互斥量，那是另一个复杂度层级。
         */
        /*
         * 先标记超时，再回收槽位。
         * 少了 reap 这一步，槽位会被 TIMEOUT 状态永久占住，
         * 表满之后 begin 一直失败 —— 网关活着但再也不发请求。
         */
        if (mg_sess_expire(&gw->sess, now_ms()) > 0)
        {
            int n = mg_sess_reap(&gw->sess, on_session_timeout, gw);

            log_warn("session timeout: %d reaped, still pending %d",
                     n, mg_sess_pending_count(&gw->sess));
        }
    }

    log_info("parse_task exit");
    vTaskDelete(NULL);
}

static void poll_task(void *p)
{
    mg_gw_t *gw = (mg_gw_t *)p;
    uint8_t slave = 1;
    uint16_t reg = 0;

    log_info("poll_task started, interval %d ms", MG_POLL_INTERVAL_MS);

    while (gw->running)
    {
        vTaskDelay(pdMS_TO_TICKS(MG_POLL_INTERVAL_MS));

        for (uint8_t i = 0; i < gw->ch_count; i++)
        {
            mg_frame_t f;
            uint16_t sid;

            /* 只有 Modbus 通道需要主站轮询；CAN 通道是被动接收 */
            if (gw->ch[i].kind != MG_KIND_MODBUS)
                continue;

            /*
             * 先占位再发送。
             * 顺序反了会漏掉「发出去之后立刻回来」的响应 ——
             * 在回环或高速总线上这是必然发生的，不是理论情况。
             */
            sid = mg_sess_begin(&gw->sess, slave, MB_FUNC_READ_HOLDING, reg,
                                now_ms(), MG_SESSION_TIMEOUT);
            if (sid == 0)
            {
                /* 表满：说明对端一直不回。不重试，等上一批超时 */
                continue;
            }

            memset(&f, 0, sizeof(f));
            f.channel = i;
            f.id = slave;
            f.dlc = MG_FRAME_DATA;
            f.ts_ms = now_ms();

            if (mg_mb_build_read(slave, MB_FUNC_READ_HOLDING, reg, f.data) != MG_OK)
            {
                mg_sess_release(&gw->sess, sid);
                continue;
            }

            if (mg_channel_send(&gw->ch[i], &f) != MG_OK)
                mg_sess_release(&gw->sess, sid);
        }

        /* 轮转：先换寄存器，一轮 4 个点走完再换从机 */
        if (++reg >= 4)
        {
            reg = 0;
            if (++slave > 2)
                slave = 1;
        }
    }

    log_info("poll_task exit");
    vTaskDelete(NULL);
}

/*
 * 处理一条下行指令。
 *
 * 指令格式（云端约定）：
 *   {"slave":1,"reg":2,"value":100}
 *
 * 只支持写单寄存器。写线圈要再定一个字段，这里不做 ——
 * 一个字段表达两种写操作，靠猜是不行的。
 */
static void handle_cmd(mg_gw_t *gw, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    cJSON *sl, *rg, *vl;

    if (o == 0)
    {
        gw->cmd_errors++;
        log_warn("cmd: json parse failed");
        return;
    }

    sl = cJSON_GetObjectItem(o, "slave");
    rg = cJSON_GetObjectItem(o, "reg");
    vl = cJSON_GetObjectItem(o, "value");

    if (!cJSON_IsNumber(sl) || !cJSON_IsNumber(rg) || !cJSON_IsNumber(vl))
    {
        gw->cmd_errors++;
        log_warn("cmd: missing slave/reg/value");
        cJSON_Delete(o);
        return;
    }

    for (uint8_t i = 0; i < gw->ch_count; i++)
    {
        mg_frame_t f;

        if (gw->ch[i].kind != MG_KIND_MODBUS)
            continue;

        memset(&f, 0, sizeof(f));
        f.channel = i;
        f.id = (uint32_t)sl->valueint;
        f.dlc = MG_FRAME_DATA;
        f.ts_ms = now_ms();

        if (mg_mb_build_write_reg((uint8_t)sl->valueint, (uint16_t)rg->valueint,
                                  (uint16_t)vl->valueint, f.data) != MG_OK)
        {
            gw->cmd_errors++;
            break;
        }

        if (mg_channel_send(&gw->ch[i], &f) == MG_OK)
        {
            gw->cmd_applied++;
            log_info("cmd: write slave %d reg %d = %d",
                     sl->valueint, rg->valueint, vl->valueint);
        }
        else
        {
            gw->uplink_errors++;
        }
        break;
    }

    cJSON_Delete(o);
}

/*
 * 通信任务：驱动重连 + 消费下行。
 *
 * 为什么不把 tick 放进 publish：connect 可能阻塞 3 秒，
 * 那会把调用 publish 的解析任务一起卡住，采集直接停摆。
 */
static void comm_task(void *p)
{
    mg_gw_t *gw = (mg_gw_t *)p;
    char buf[MG_DOWNLINK_MAX];

    log_info("comm_task started");

    while (gw->running)
    {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (gw->uplink != 0 && gw->uplink->tick != 0)
            gw->uplink->tick();

        if (mg_uplink_poll_cmd(buf, sizeof(buf)) > 0)
        {
            gw->cmd_received++;
            handle_cmd(gw, buf);
        }
    }

    log_info("comm_task exit");
    vTaskDelete(NULL);
}

static void monitor_task(void *p)
{
    mg_gw_t *gw = (mg_gw_t *)p;

    while (gw->running)
    {
        vTaskDelay(pdMS_TO_TICKS(MG_MONITOR_PERIOD_MS));
        mg_gw_print_stats(gw);
    }

    vTaskDelete(NULL);
}

/* ---------- 对外接口 ---------- */

int mg_gw_init(mg_gw_t *gw, const mg_uplink_ops_t *up)
{
    if (gw == 0)
        return MG_ERR_PARAM;

    memset(gw, 0, sizeof(*gw));
    gw->uplink = up;
    mg_sess_init(&gw->sess);

    /*
     * 队列元素是 24 字节的结构体，深度 32 约 800 字节。
     * 在 20KB RAM 的 F103 上这是要算进预算的，不能随手开 256。
     */
    gw->rx_q = xQueueCreate(MG_RX_QUEUE_DEPTH, sizeof(mg_frame_t));
    if (gw->rx_q == 0)
        return MG_ERR_STATE;

    gw->running = 0;
    return MG_OK;
}

int mg_gw_add_channel(mg_gw_t *gw, const char *name, mg_chan_kind_t kind,
                      const char *arg, uint8_t *out_index)
{
    const mg_channel_ops_t *ops;
    mg_channel_t *ch;
    uint8_t idx;

    if (gw == 0 || gw->ch_count >= MG_CHANNEL_MAX)
        return MG_ERR_PARAM;

    idx = gw->ch_count;
    ch = &gw->ch[idx];
    ch->index = idx;

    if (kind == MG_KIND_CAN)
        ops = mg_chan_socketcan_ops();
    else
        ops = mg_chan_loopback_ops();

    if (mg_channel_open(ch, name, kind, ops, arg) != MG_OK)
        return MG_ERR_IO;

    gw->ch_count++;

    if (out_index != 0)
        *out_index = idx;

    return MG_OK;
}

int mg_gw_start(mg_gw_t *gw)
{
    if (gw == 0 || gw->ch_count == 0)
        return MG_ERR_PARAM;

    gw->running = 1;

    if (gw->uplink != 0 && gw->uplink->open != 0)
    {
        if (gw->uplink->open() != 0)
            log_warn("uplink open failed, data will be dropped");
    }

    /*
     * 每个通道一个接收任务。优先级定为 3（高于 parse 的 2）：
     * 收帧必须及时，否则硬件 FIFO 溢出后丢的是整帧；
     * 解析稍慢只会让队列变长。
     */
    for (uint8_t i = 0; i < gw->ch_count; i++)
    {
        g_rx_arg[i].gw = gw;
        g_rx_arg[i].idx = i;

        if (xTaskCreate(rx_task, "rx", 1024, &g_rx_arg[i], 3, &gw->rx_task[i]) != pdPASS)
            return MG_ERR_STATE;
    }

    if (xTaskCreate(parse_task, "parse", 2048, gw, 2, &gw->parse_task) != pdPASS)
        return MG_ERR_STATE;

    if (xTaskCreate(poll_task, "poll", 1024, gw, 1, &gw->poll_task) != pdPASS)
        return MG_ERR_STATE;

    if (xTaskCreate(monitor_task, "mon", 1024, gw, 1, &gw->monitor_task) != pdPASS)
        return MG_ERR_STATE;

    if (xTaskCreate(comm_task, "comm", 1536, gw, 1, &gw->comm_task) != pdPASS)
        return MG_ERR_STATE;

    return MG_OK;
}

void mg_gw_stop(mg_gw_t *gw)
{
    if (gw == 0)
        return;

    gw->running = 0;

    /* 给任务一点时间退出。真实固件里通常直接复位，不需要这一步 */
    vTaskDelay(pdMS_TO_TICKS(200));
}

void mg_gw_print_stats(mg_gw_t *gw)
{
    if (gw == 0)
        return;

    log_info("stats: rx_drop=%u parse_err=%u json_err=%u up_err=%u "
             "sess start=%u done=%u timeout=%u unmatched=%u "
             "cmd rx=%u ok=%u err=%u",
             (unsigned)gw->rx_dropped, (unsigned)gw->parse_errors,
             (unsigned)gw->json_errors, (unsigned)gw->uplink_errors,
             (unsigned)gw->sess.started, (unsigned)gw->sess.completed,
             (unsigned)gw->sess.timedout, (unsigned)gw->sess.unmatched,
             (unsigned)gw->cmd_received, (unsigned)gw->cmd_applied,
             (unsigned)gw->cmd_errors);

    for (uint8_t i = 0; i < gw->ch_count; i++)
    {
        log_info("  ch[%u] %-6s rx=%u tx=%u rx_err=%u tx_err=%u",
                 i, gw->ch[i].name, (unsigned)gw->ch[i].rx_frames,
                 (unsigned)gw->ch[i].tx_frames, (unsigned)gw->ch[i].rx_errors,
                 (unsigned)gw->ch[i].tx_errors);
    }

    /*
     * 栈水位：单位是「字」（word），不是字节。
     * 这个数字是给 MCU 调栈大小的依据 —— 原课程项目里完全没有，
     * 上线后栈溢出只会表现为随机死机，查不出来。
     */
    for (uint8_t i = 0; i < gw->ch_count; i++)
        if (gw->rx_task[i] != 0)
            log_info("  stack rx[%u] free=%u words", i,
                     (unsigned)uxTaskGetStackHighWaterMark(gw->rx_task[i]));

    if (gw->parse_task != 0)
        log_info("  stack parse free=%u words",
                 (unsigned)uxTaskGetStackHighWaterMark(gw->parse_task));
}
