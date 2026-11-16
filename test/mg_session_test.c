/*
 * mg_session_test.c —— 会话表
 *
 * 重点验证「课程项目里出错的那几件事」：
 *   - 响应必须按 FIFO 匹配，不能按数组顺序
 *   - 超时必须能收割，不能永久悬挂
 *   - 表满必须明确失败，不能驱逐在途会话
 *   - 时间用传入值，测试不用真等 500 毫秒
 */
#include <stdio.h>
#include <string.h>
#include "mg_session.h"

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

static void test_begin_and_get(void)
{
    mg_sess_tbl_t t;
    uint16_t s1, s2;
    mg_session_t *s;

    printf("\nBegin / get\n");
    mg_sess_init(&t);

    s1 = mg_sess_begin(&t, 1, 3, 0, 1000, 500);
    s2 = mg_sess_begin(&t, 2, 3, 1, 1000, 500);

    check(s1 != 0, "first sid is valid");
    check(s2 != 0, "second sid is valid");
    check(s1 != s2, "sids are distinct");
    check(s2 == s1 + 1, "sids increase by one");

    s = mg_sess_get(&t, s1);
    check(s != 0, "session retrievable by sid");
    check(s != 0 && s->slave == 1 && s->func == 3 && s->reg == 0, "session fields stored");
    check(mg_sess_pending_count(&t) == 2, "two pending sessions");

    check(mg_sess_get(&t, 0) == 0, "sid 0 is never valid");
    check(mg_sess_get(&t, 999) == 0, "unknown sid returns null");
}

static void test_match_fifo(void)
{
    mg_sess_tbl_t t;
    uint16_t s1, s2, hit;
    mg_session_t *s;

    printf("\nMatch is FIFO, not array order\n");
    mg_sess_init(&t);

    s1 = mg_sess_begin(&t, 5, 3, 10, 1000, 500);
    s2 = mg_sess_begin(&t, 5, 3, 20, 1000, 500);

    /* 先返回的响应应该给先发出的那个请求 */
    hit = mg_sess_on_response(&t, 5, 3, 111, 0);
    check(hit == s1, "first response goes to earliest session");

    s = mg_sess_get(&t, s1);
    check(s != 0 && s->state == MG_SESS_DONE && s->value == 111, "earliest marked done");

    s = mg_sess_get(&t, s2);
    check(s != 0 && s->state == MG_SESS_PENDING, "later session still pending");

    hit = mg_sess_on_response(&t, 5, 3, 222, 0);
    check(hit == s2, "second response goes to second session");

    /* 第三个响应没有对应请求 */
    hit = mg_sess_on_response(&t, 5, 3, 333, 0);
    check(hit == 0, "extra response not matched");
    check(t.unmatched == 1, "unmatched counted");
}

static void test_different_slave(void)
{
    mg_sess_tbl_t t;
    uint16_t a, b, hit;

    printf("\nMatch keyed on (slave, func)\n");
    mg_sess_init(&t);

    a = mg_sess_begin(&t, 1, 3, 0, 1000, 500);
    b = mg_sess_begin(&t, 2, 3, 0, 1000, 500);

    hit = mg_sess_on_response(&t, 2, 3, 7, 0);
    check(hit == b, "response matched to its own slave");

    hit = mg_sess_on_response(&t, 1, 4, 7, 0);
    check(hit == 0, "wrong function does not match");

    hit = mg_sess_on_response(&t, 1, 3, 7, 0);
    check(hit == a, "correct slave+func matches");
}

static void test_timeout(void)
{
    mg_sess_tbl_t t;
    uint16_t s;
    mg_session_t *sess;
    int n;

    printf("\nTimeout\n");
    mg_sess_init(&t);

    s = mg_sess_begin(&t, 1, 3, 0, 5000, 500);

    check(mg_sess_expire(&t, 5400) == 0, "not expired before deadline");
    sess = mg_sess_get(&t, s);
    check(sess != 0 && sess->state == MG_SESS_PENDING, "still pending");

    n = mg_sess_expire(&t, 5500)
        ;   /* 恰好到点：>= 判定应当超时 */
    check(n == 1, "expires exactly at deadline");
    sess = mg_sess_get(&t, s);
    check(sess != 0 && sess->state == MG_SESS_TIMEOUT, "state is timeout");
    check(t.timedout == 1, "timeout counted");

    /* 已超时的会话不再被响应匹配 */
    check(mg_sess_on_response(&t, 1, 3, 1, 0) == 0, "timed-out session not matched");

    /* 释放后槽位可复用 */
    mg_sess_release(&t, s);
    check(mg_sess_get(&t, s) == 0, "released session gone");
    check(mg_sess_begin(&t, 1, 3, 0, 6000, 500) != 0, "slot reusable after release");
}

/* reap 回调：统计被回收的会话 */
static int g_reaped;
static uint16_t g_reaped_sid;

static void on_reap(const mg_session_t *s, void *user)
{
    (void)user;
    g_reaped++;
    g_reaped_sid = s->sid;
}

static void test_reap(void)
{
    mg_sess_tbl_t t;
    uint16_t a, b;
    int n;

    printf("\nReap timed-out slots\n");
    mg_sess_init(&t);

    a = mg_sess_begin(&t, 1, 3, 0, 1000, 100);
    b = mg_sess_begin(&t, 2, 3, 1, 1000, 5000);

    check(mg_sess_expire(&t, 1200) == 1, "only the short one expires");

    /* expire 只标记，槽位仍占着 —— 这时还能查到，状态是 TIMEOUT */
    check(mg_sess_get(&t, a) != 0, "timed-out session still present before reap");

    g_reaped = 0;
    n = mg_sess_reap(&t, on_reap, 0);
    check(n == 1, "one slot reaped");
    check(g_reaped == 1 && g_reaped_sid == a, "callback sees the timed-out sid");

    check(mg_sess_get(&t, a) == 0, "slot released after reap");

    /*
     * 关键：回收之后必须能重新开。
     * 少了 reap，这里会一直失败 —— 网关看似在跑，实际发不出请求。
     */
    check(mg_sess_begin(&t, 1, 3, 0, 2000, 100) != 0, "slot reusable after reap");

    /* 未超时的那个不能被回收 */
    n = mg_sess_reap(&t, on_reap, 0);
    check(n == 0, "pending session untouched by reap");
    check(mg_sess_get(&t, b) != 0, "pending session still present");
}

static void test_time_wraparound(void)
{
    mg_sess_tbl_t t;
    uint16_t s;
    mg_session_t *sess;

    printf("\nTime wraparound\n");
    mg_sess_init(&t);

    /* 起点接近 32 位上限，超时判断必须能跨过回绕点 */
    s = mg_sess_begin(&t, 1, 3, 0, 0xFFFFFF00u, 500);
    check(mg_sess_expire(&t, 0x00000010u) == 0,
          "wrap-around does not falsely expire (elapsed 272ms)");

    sess = mg_sess_get(&t, s);
    check(sess != 0 && sess->state == MG_SESS_PENDING, "still pending after wrap");

    check(mg_sess_expire(&t, 0x00000100u) == 1,
          "expires after wrap when elapsed exceeds timeout");
}

static void test_table_full(void)
{
    mg_sess_tbl_t t;
    int i;
    int ok = 0;

    printf("\nTable full\n");
    mg_sess_init(&t);

    for (i = 0; i < MG_SESSION_MAX; i++)
        if (mg_sess_begin(&t, 1, 3, (uint16_t)i, 1000, 500) != 0)
            ok++;

    check(ok == MG_SESSION_MAX, "all slots usable");
    check(mg_sess_begin(&t, 1, 3, 0, 1000, 500) == 0, "begin fails when full");
    check(mg_sess_pending_count(&t) == MG_SESSION_MAX, "existing sessions untouched");

    /* 释放一个即可再开 */
    {
        mg_session_t *s = mg_sess_get(&t, 1);
        check(s != 0, "first slot exists");
        mg_sess_release(&t, 1);
    }
    check(mg_sess_begin(&t, 9, 3, 0, 1000, 500) != 0, "slot available after release");
}

static void test_sid_wrap(void)
{
    mg_sess_tbl_t t;
    uint16_t s;

    printf("\nSid wraparound skips zero\n");
    mg_sess_init(&t);

    t.next_sid = 0xFFFF;
    s = mg_sess_begin(&t, 1, 3, 0, 1000, 500);
    check(s == 0xFFFF, "sid reaches max");

    s = mg_sess_begin(&t, 1, 3, 0, 1000, 500);
    check(s == 1, "sid wraps to 1, skipping 0");

    check(mg_sess_get(&t, 0) == 0, "0 never allocated");
}

int main(void)
{
    printf("=== mg_session_test ===\n");

    test_begin_and_get();
    test_match_fifo();
    test_different_slave();
    test_timeout();
    test_reap();
    test_time_wraparound();
    test_table_full();
    test_sid_wrap();

    printf("\nResult: %d checks, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
