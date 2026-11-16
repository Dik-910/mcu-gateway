/*
 * mg_flow_test.c —— 端到端流程（跑在真实 FreeRTOS 上）
 *
 * 两个阶段：
 *   A. 从机正常应答 -> 会话全部完成，没有超时
 *   B. 从机不再应答 -> 会话必须超时，不能永久悬挂
 *
 * 阶段 B 是整个项目要证明的核心：课程项目在同样的情况下会
 * 「看起来还在跑，其实再也不上报」，这里必须表现为可观测的超时计数。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "mg_app.h"
#include "mg_modbus.h"
#include "log.h"

static int g_fail;
static int g_total;

static volatile int g_slave_answers = 1;
static uint32_t g_b_started;  /* 阶段 B 结束时的请求数 */
static mg_gw_t g_gw;

static void check(int cond, const char *what)
{
    g_total++;
    if (!cond)
    {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void slave_task(void *p)
{
    mg_gw_t *gw = (mg_gw_t *)p;
    mg_frame_t req;

    for (;;)
    {
        if (mg_chan_loopback_take(&gw->ch[0], &req) == MG_OK)
        {
            if (g_slave_answers)
            {
                uint8_t resp[7];
                uint16_t reg = (uint16_t)((req.data[2] << 8) | req.data[3]);
                uint16_t val = (uint16_t)(reg * 10);
                uint16_t crc;
                mg_frame_t rsp;

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
            /* 不回的时候什么都不做 —— 模拟从机掉线或地址配错 */
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/* 阶段 1 结束时的检查 */
static void phase_a_checks(void)
{
    printf("\nPhase A: slave answers\n");
    printf("  started=%u completed=%u timedout=%u unmatched=%u pending=%d\n",
           (unsigned)g_gw.sess.started, (unsigned)g_gw.sess.completed,
           (unsigned)g_gw.sess.timedout, (unsigned)g_gw.sess.unmatched,
           mg_sess_pending_count(&g_gw.sess));
    check(g_gw.sess.started > 0, "requests were sent");
    check(g_gw.sess.completed > 0, "responses were matched");
    check(g_gw.sess.timedout == 0, "no timeout while slave answers");
    check(g_gw.sess.unmatched == 0, "every response matched a session");
    check(g_gw.parse_errors == 0, "no parse errors");
    check(g_gw.uplink_errors == 0, "no uplink errors");
    check(g_gw.rx_dropped == 0, "no frames dropped while healthy");
    check(mg_sess_pending_count(&g_gw.sess) == 0, "no session left pending");
}

static void phase_b_checks(void)
{
    printf("\nPhase B: slave silent\n");
    printf("  started=%u completed=%u timedout=%u unmatched=%u pending=%d\n",
           (unsigned)g_gw.sess.started, (unsigned)g_gw.sess.completed,
           (unsigned)g_gw.sess.timedout, (unsigned)g_gw.sess.unmatched,
           mg_sess_pending_count(&g_gw.sess));
    check(g_gw.sess.timedout > 0, "silent slave causes timeouts");
    check(g_gw.sess.unmatched == 0, "still no mismatched responses");

    /*
     * 关键：超时之后槽位必须能回收，否则表会被占满，
     * 之后即使从机恢复也发不出新请求 —— 这正是原项目「永久卡死」的机理。
     */
    check(mg_sess_pending_count(&g_gw.sess) < MG_SESSION_MAX,
          "expired sessions were reclaimed, table not permanently full");

    g_b_started = g_gw.sess.started;
}

static void driver_task(void *p)
{
    (void)p;

    vTaskDelay(pdMS_TO_TICKS(1500));
    phase_a_checks();

    g_slave_answers = 0;
    vTaskDelay(pdMS_TO_TICKS(1500));
    phase_b_checks();

    /* 让从机恢复，验证还能继续工作 */
    g_slave_answers = 1;
    vTaskDelay(pdMS_TO_TICKS(800));
    printf("\nPhase C: slave recovered\n");
    printf("  started=%u completed=%u timedout=%u unmatched=%u pending=%d\n",
           (unsigned)g_gw.sess.started, (unsigned)g_gw.sess.completed,
           (unsigned)g_gw.sess.timedout, (unsigned)g_gw.sess.unmatched,
           mg_sess_pending_count(&g_gw.sess));
    /*
     * 真正要证的是「还能发出新请求」。
     * 如果超时槽位没被回收，表会一直满，started 停在这里不再增长 ——
     * 而 completed > 0 依然成立（历史值），断言会假通过。
     */
    check(g_gw.sess.started > g_b_started,
          "new requests were sent after recovery (slots were reclaimed)");
    check(g_gw.sess.completed > 14, "gateway resumes after slave recovers");

    mg_gw_stop(&g_gw);
    vTaskEndScheduler();
    vTaskDelete(NULL);
}

int main(void)
{
    uint8_t idx;

    log_set_level(LOG_ERROR);

    printf("=== mg_flow_test (real FreeRTOS) ===\n");

    if (mg_gw_init(&g_gw, mg_uplink_stdout_ops()) != MG_OK)
    {
        printf("init failed\n");
        return 1;
    }

    if (mg_gw_add_channel(&g_gw, "loop0", MG_KIND_MODBUS, "loop", &idx) != MG_OK)
    {
        printf("add channel failed\n");
        return 1;
    }

    if (mg_gw_start(&g_gw) != MG_OK)
    {
        printf("start failed\n");
        return 1;
    }

    xTaskCreate(slave_task, "slave", 1024, &g_gw, 2, NULL);
    xTaskCreate(driver_task, "drv", 1024, &g_gw, 2, NULL);

    vTaskStartScheduler();

    printf("\nResult: %d checks, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
