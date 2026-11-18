/*
 * mg_uplink.h —— 上行通道抽象
 *
 * 上行负责「把数据发出去」，同时还承担「把云端指令收进来」——
 * 这两件事在 MQTT 里是同一个连接，拆两个模块反而要维护两条连接。
 *
 * 下行不能用回调直接调 FreeRTOS API：
 * Paho 的消息回调跑在它自己的线程里（PC 上）或需要主动 yield（MCU 上），
 * 都不是 RTOS 任务上下文。在里面调用 xQueueSend 在 POSIX 移植上也许能跑，
 * 到 Cortex-M 上直接崩 —— 那种「PC 上没事、板子上死机」的 bug 最难查。
 *
 * 所以下行数据走一个单生产者/单消费者环形缓冲：
 *   生产者 = MQTT 回调（写 head）
 *   消费者 = cmd_task（写 tail）
 * 只推进各自的 32 位索引，不需要锁。
 */
#ifndef MG_UPLINK_H
#define MG_UPLINK_H

#include <stdint.h>

#define MG_DOWNLINK_MAX  256   /* 单条下行指令最大长度 */

typedef struct
{
    int  (*open)(void);
    /*
     * 返回 0 表示**已被接受**（MQTT 实现里是「已落盘入队」），
     * <0 表示没接住（超长、后端写失败），调用方计数即可，不要在这里重试。
     *
     * 「已接受」不等于「已投递」：投递语义由实现决定，MQTT 侧是
     * QoS1 + PUBACK 确认，断线期间的数据留在 outbox 里等重连。
     */
    int  (*publish)(const char *json);
    /*
     * 可选：驱动内部维护（重连、保活）。
     * 由 RTOS 任务周期调用 —— 不在 publish 里顺手重连，
     * 因为 connect 可能阻塞数秒，会把解析任务一起卡住。
     */
    void (*tick)(void);
    void (*close)(void);
} mg_uplink_ops_t;

/* stdout 实现：用于演示与测试，不依赖任何网络 */
const mg_uplink_ops_t *mg_uplink_stdout_ops(void);

/* MQTT 实现：基于 Paho C。没有 broker 时 open 会失败但编译不受影响 */
const mg_uplink_ops_t *mg_uplink_mqtt_ops(void);

/*
 * 取一条下行指令（以 '\0' 结尾的 JSON）。
 * 返回长度，0 表示当前没有指令。由 cmd_task 轮询调用。
 */
int mg_uplink_poll_cmd(char *buf, int len);

/*
 * 注入一条下行指令。
 *
 * 生产路径是 MQTT 回调自己调它；公开出来是为了让测试和演示
 * 在没有 broker 的情况下也能验证下行处理链路。
 * 队列满时返回 -1，不阻塞 —— 调用方可能跑在网络回调里。
 */
int mg_uplink_push_cmd(const char *json);

#endif /* MG_UPLINK_H */
