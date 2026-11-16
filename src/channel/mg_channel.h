/*
 * mg_channel.h —— 通道抽象
 *
 * 为什么要有这一层：
 * 网关的本职是「多路接入」，但每路的物理实现完全不同 ——
 * CAN 走 SocketCAN / bxCAN，Modbus 走串口，将来还要加 LoRa。
 * 如果上层直接访问某个具体设备，换一路就要改一遍业务代码。
 *
 * 所以这里只定义四个动作：打开、收、发、关闭。
 * 上层任务拿到的永远只是 mg_frame_t，不知道底下是 CAN 还是串口。
 *
 * 与 can-node 的 can_driver_ops_t 是同一个思路，区别在于：
 * 那边的实现是阻塞/轮询，这里的 recv 必须非阻塞 ——
 * RTOS 里阻塞读会占满一个任务的栈和优先级，而任务数量是有限的。
 */
#ifndef MG_CHANNEL_H
#define MG_CHANNEL_H

#include "mg_types.h"

typedef struct mg_channel mg_channel_t;

typedef struct
{
    int  (*open)(mg_channel_t *ch, const char *arg);
    /* 非阻塞收：有帧返回 MG_OK，无帧返回 MG_ERR_NOMSG */
    int  (*recv)(mg_channel_t *ch, mg_frame_t *f);
    int  (*send)(mg_channel_t *ch, const mg_frame_t *f);
    void (*close)(mg_channel_t *ch);
} mg_channel_ops_t;

struct mg_channel
{
    char name[MG_NAME_MAX];          /* 日志用的短名，如 "can0" */
    mg_chan_kind_t kind;
    const mg_channel_ops_t *ops;
    void *ctx;                       /* 实现的私有状态 */
    uint8_t index;                   /* 全局唯一索引，进队列时带上 */

    /* 统计：每路独立计数，便于定位是哪一路出问题 */
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_errors;
    uint32_t tx_errors;
};

/* 打开通道。arg 的含义由实现决定（CAN 是接口名，Modbus 是设备路径） */
int mg_channel_open(mg_channel_t *ch, const char *name, mg_chan_kind_t kind,
                    const mg_channel_ops_t *ops, const char *arg);

int mg_channel_recv(mg_channel_t *ch, mg_frame_t *f);
int mg_channel_send(mg_channel_t *ch, const mg_frame_t *f);
void mg_channel_close(mg_channel_t *ch);

/* 各实现的 ops 入口 */
const mg_channel_ops_t *mg_chan_loopback_ops(void);   /* 内存回环，用于测试 */
const mg_channel_ops_t *mg_chan_socketcan_ops(void);  /* Linux SocketCAN */

/*
 * 回环通道的测试接口，模拟「对端设备」的两个方向。
 * 生产代码不该调用它们，只有测试和演示会用到。
 *
 *   inject：对端发一帧给网关（网关会在 recv 里收到）
 *   take  ：对端取走网关发出去的一帧
 */
int mg_chan_loopback_inject(mg_channel_t *ch, const mg_frame_t *f);
int mg_chan_loopback_take(mg_channel_t *ch, mg_frame_t *f);

#endif /* MG_CHANNEL_H */
