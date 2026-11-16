/*
 * mg_types.h —— 网关内部统一的数据类型
 *
 * 设计原则：所有跨任务传递的数据都是定长的，不含指针。
 * 因为队列是拷贝语义，传指针就要管生命周期，在 MCU 上很容易泄漏。
 */
#ifndef MG_TYPES_H
#define MG_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define MG_CHANNEL_MAX   4     /* 最多几路通道 */
#define MG_FRAME_DATA    8     /* 单帧最大数据字节（CAN 上限；串口复用同一结构） */
#define MG_NAME_MAX      16

/* 通道类型决定解析方式，不是物理介质 */
typedef enum
{
    MG_KIND_CAN = 0,      /* CAN 帧，ID 为 11 位标准帧 */
    MG_KIND_MODBUS = 1,   /* Modbus RTU 帧，ID 为从机地址 */
} mg_chan_kind_t;

/* 错误码：负值表示错误，0 表示成功 */
typedef enum
{
    MG_OK        = 0,
    MG_ERR_PARAM = -1,
    MG_ERR_STATE = -2,
    MG_ERR_NOMSG = -3,    /* 暂无数据，不是故障 —— 上层不能当错误处理 */
    MG_ERR_FULL  = -4,
    MG_ERR_IO    = -5,
} mg_status_t;

/*
 * 一帧数据。
 *
 * 字段顺序刻意让结构体大小为 24 字节（4 字节对齐），
 * 在 Cortex-M 上队列拷贝一次 6 个字，不会退化成逐字节拷贝。
 *
 * ts_ms 是接收时刻，用 tick 换算。会话超时靠它判断，
 * 不用 xTaskGetTickCount() 现取 —— 否则队列积压时会把
 * 「入队时间」和「处理时间」混为一谈，超时判断失准。
 */
typedef struct
{
    uint32_t id;                /* CAN 标准帧 ID 或 Modbus 从机地址 */
    uint32_t ts_ms;             /* 入队时刻（毫秒） */
    uint8_t  channel;           /* 来源通道索引 */
    uint8_t  dlc;               /* 有效数据字节数 */
    uint8_t  data[MG_FRAME_DATA];
    uint8_t  _pad[2];           /* 显式补齐，避免不同编译器产生不同布局 */
} mg_frame_t;

#endif /* MG_TYPES_H */
