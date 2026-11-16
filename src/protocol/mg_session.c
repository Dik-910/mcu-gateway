/*
 * mg_session.c —— 会话表实现
 */
#include "mg_session.h"
#include <string.h>

void mg_sess_init(mg_sess_tbl_t *t)
{
    if (t == 0)
        return;

    memset(t, 0, sizeof(*t));
    t->next_sid = 1;
}

uint16_t mg_sess_begin(mg_sess_tbl_t *t, uint8_t slave, uint8_t func,
                       uint16_t reg, uint32_t now_ms, uint32_t timeout_ms)
{
    mg_session_t *s = 0;
    uint16_t sid;

    if (t == 0)
        return 0;

    for (int i = 0; i < MG_SESSION_MAX; i++)
    {
        if (t->slot[i].state == MG_SESS_FREE)
        {
            s = &t->slot[i];
            break;
        }
    }

    /* 表满直接失败。不驱逐旧会话：驱逐会让上层拿到错配的响应 */
    if (s == 0)
        return 0;

    sid = t->next_sid++;
    if (t->next_sid == 0)
        t->next_sid = 1;   /* sid 0 保留为无效，回绕时跳过 */

    memset(s, 0, sizeof(*s));
    s->sid = sid;
    s->slave = slave;
    s->func = func;
    s->reg = reg;
    s->sent_ms = now_ms;
    s->timeout_ms = (timeout_ms != 0) ? timeout_ms : MG_SESSION_TIMEOUT;
    s->state = MG_SESS_PENDING;

    t->started++;
    return sid;
}

uint16_t mg_sess_on_response(mg_sess_tbl_t *t, uint8_t slave, uint8_t func,
                             uint16_t value, uint8_t exc)
{
    mg_session_t *hit = 0;

    if (t == 0)
        return 0;

    /*
     * 匹配规则：同 (slave, func) 中「sid 最小」即最早发出的那个。
     *
     * 为什么不能只取第一个找到的？因为槽位是复用的，数组顺序不代表
     * 发出顺序。用 sid 比较才能保证 FIFO —— 单主站顺序轮询时这不会出问题，
     * 但只要有两个请求同时在途（比如重发），按数组顺序匹配就可能错配。
     */
    for (int i = 0; i < MG_SESSION_MAX; i++)
    {
        mg_session_t *s = &t->slot[i];

        if (s->state != MG_SESS_PENDING)
            continue;
        if (s->slave != slave || s->func != func)
            continue;

        if (hit == 0 || s->sid < hit->sid)
            hit = s;
    }

    if (hit == 0)
    {
        t->unmatched++;
        return 0;
    }

    hit->value = value;
    hit->exc = exc;
    hit->state = MG_SESS_DONE;
    t->completed++;
    return hit->sid;
}

int mg_sess_expire(mg_sess_tbl_t *t, uint32_t now_ms)
{
    int n = 0;

    if (t == 0)
        return 0;

    for (int i = 0; i < MG_SESSION_MAX; i++)
    {
        mg_session_t *s = &t->slot[i];

        if (s->state != MG_SESS_PENDING)
            continue;

        /*
         * 用无符号减法算经过时间，32 位回绕也能正确比较。
         * 不写 now_ms > sent_ms + timeout：那种写法在 tick 回绕时会漏判。
         */
        if ((uint32_t)(now_ms - s->sent_ms) >= s->timeout_ms)
        {
            s->state = MG_SESS_TIMEOUT;
            t->timedout++;
            n++;
        }
    }

    return n;
}

int mg_sess_reap(mg_sess_tbl_t *t,
                 void (*cb)(const mg_session_t *s, void *user), void *user)
{
    int n = 0;

    if (t == 0)
        return 0;

    for (int i = 0; i < MG_SESSION_MAX; i++)
    {
        mg_session_t *s = &t->slot[i];

        if (s->state != MG_SESS_TIMEOUT)
            continue;

        if (cb != 0)
            cb(s, user);

        s->state = MG_SESS_FREE;
        n++;
    }

    return n;
}

mg_session_t *mg_sess_get(mg_sess_tbl_t *t, uint16_t sid)
{
    if (t == 0 || sid == 0)
        return 0;

    for (int i = 0; i < MG_SESSION_MAX; i++)
    {
        if (t->slot[i].sid == sid && t->slot[i].state != MG_SESS_FREE)
            return &t->slot[i];
    }

    return 0;
}

void mg_sess_release(mg_sess_tbl_t *t, uint16_t sid)
{
    mg_session_t *s = mg_sess_get(t, sid);

    if (s != 0)
        s->state = MG_SESS_FREE;
}

int mg_sess_pending_count(const mg_sess_tbl_t *t)
{
    int n = 0;

    if (t == 0)
        return 0;

    for (int i = 0; i < MG_SESSION_MAX; i++)
        if (t->slot[i].state == MG_SESS_PENDING)
            n++;

    return n;
}
