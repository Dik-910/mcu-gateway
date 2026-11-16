/*
 * mg_session.h —— 请求会话表
 *
 * 这一层是为了修掉课程项目里最危险的一个缺陷。
 *
 * 原项目的写法是：用一个静态的 cJSON 对象攒数据，靠
 * 「这个字段是否已存在」判断当前是第几次响应。
 *
 *   if (字段已存在) 填第二个字段; else 填第一个字段;
 *
 * 问题在于：它没有事务标识、没有超时、没有清理。
 * 只要五个响应里丢一帧，这个对象就永远凑不齐且不会被回收 ——
 * 之后所有请求都会往同一个残缺对象上叠加，网关看起来还在运行，
 * 实际上再也不会向上报一次正确的数据。
 *
 * 正确做法是把「一次请求-响应」显式建模成一个会话：
 *   - 发请求时占一个槽位，拿到自增 sid
 *   - 响应到达时按 (从机, 功能码) 找到最早的那个 PENDING 会话
 *   - 超时由 expire 统一收割，不会永久悬挂
 *   - 槽位数量有上限，满了就明确失败，而不是悄悄复用别人的状态
 */
#ifndef MG_SESSION_H
#define MG_SESSION_H

#include <stdint.h>

#define MG_SESSION_MAX      8    /* 槽位数：够覆盖「一个轮询周期内的在途请求」即可 */
#define MG_SESSION_TIMEOUT  500  /* 默认超时（毫秒），Modbus 一般 100~1000 */

typedef enum
{
    MG_SESS_FREE = 0,
    MG_SESS_PENDING,   /* 已发出，等待响应 */
    MG_SESS_DONE,      /* 收到正常响应 */
    MG_SESS_TIMEOUT,   /* 超时，等上层收割 */
} mg_sess_state_t;

typedef struct
{
    uint16_t sid;         /* 事务号，0 表示无效 */
    uint8_t  slave;
    uint8_t  func;
    uint16_t reg;
    uint16_t value;       /* 响应值 */
    uint8_t  exc;         /* 异常码，0 表示正常 */
    uint8_t  state;
    uint32_t sent_ms;     /* 发出时刻，用于算超时 */
    uint32_t timeout_ms;
} mg_session_t;

typedef struct
{
    mg_session_t slot[MG_SESSION_MAX];
    uint16_t next_sid;

    /* 统计：用来回答「丢失率多少」这种问题 */
    uint32_t started;
    uint32_t completed;
    uint32_t timedout;
    uint32_t unmatched;   /* 响应找不到对应会话 —— 说明对端超时后又回了 */
} mg_sess_tbl_t;

void mg_sess_init(mg_sess_tbl_t *t);

/*
 * 开一个会话。返回 sid，0 表示表满（上层必须处理，不能忽略）。
 * now_ms 是请求发出的时刻，由调用方传入而不是内部取 ——
 * 这样测试可以精确控制时间，不用真的等 500 毫秒。
 */
uint16_t mg_sess_begin(mg_sess_tbl_t *t, uint8_t slave, uint8_t func,
                       uint16_t reg, uint32_t now_ms, uint32_t timeout_ms);

/*
 * 收到响应时调用。
 * 按 (slave, func) 匹配「最早发出且仍在等待」的会话，返回其 sid；
 * 找不到返回 0（计入 unmatched）。
 */
uint16_t mg_sess_on_response(mg_sess_tbl_t *t, uint8_t slave, uint8_t func,
                             uint16_t value, uint8_t exc);

/*
 * 标记超时：把过期的 PENDING 置为 TIMEOUT，返回数量。
 * 不在这里释放，是因为超时事件本身要上报（"这个从机没响应"）——
 * 释放了上层就没机会知道发生过超时。
 */
int mg_sess_expire(mg_sess_tbl_t *t, uint32_t now_ms);

/*
 * 回收已标记为 TIMEOUT 的槽位，逐个回调（带出 sid/从机/寄存器），然后置 FREE。
 *
 * 必须调用它，否则槽位永久泄漏：超时的会话不会自己变回 FREE，
 * 表被占满之后 begin 一直失败，网关看似在运行，实际再也发不出请求 ——
 * 这与课程项目「凑不齐字段就永久卡住」是同一类故障，只是位置从数据换到了槽位。
 */
int mg_sess_reap(mg_sess_tbl_t *t,
                 void (*cb)(const mg_session_t *s, void *user), void *user);

mg_session_t *mg_sess_get(mg_sess_tbl_t *t, uint16_t sid);

/* 上层处理完（无论成功、异常、超时）后释放槽位 */
void mg_sess_release(mg_sess_tbl_t *t, uint16_t sid);

/* 当前在途会话数 */
int mg_sess_pending_count(const mg_sess_tbl_t *t);

#endif /* MG_SESSION_H */
