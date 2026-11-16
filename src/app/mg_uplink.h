/*
 * mg_uplink.h —— 上行通道抽象
 *
 * 为什么单独抽一层：上行的「目的地」和协议是两件事。
 * 演示时打到 stdout，部署时发 MQTT，将来可能是 LoRa 透传或串口上报。
 * 如果业务代码里直接调 MQTTPublish，换一个目的地就要改一遍业务逻辑。
 *
 * 与通道层不同的是：这里只有一个动作 —— publish 一段 JSON。
 * 因为上行不需要「收」，订阅指令走的是另一个方向，由 controller 处理。
 */
#ifndef MG_UPLINK_H
#define MG_UPLINK_H

#include <stdint.h>

typedef struct
{
    int  (*open)(void);
    /* 返回 0 表示已交给下层；<0 表示失败，调用方应计数但不要阻塞重试 */
    int  (*publish)(const char *json);
    void (*close)(void);
} mg_uplink_ops_t;

/* stdout 实现：用于演示与测试，不依赖任何网络 */
const mg_uplink_ops_t *mg_uplink_stdout_ops(void);

/*
 * MQTT 实现：基于 Paho C。
 * 没有 broker 时 open 会失败，但编译不受影响 ——
 * 这样仓库在任何机器上都能构建，联调时再装 broker。
 */
const mg_uplink_ops_t *mg_uplink_mqtt_ops(void);

#endif /* MG_UPLINK_H */
