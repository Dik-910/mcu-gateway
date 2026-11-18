/*
 * mg_param.h —— 掉电不丢的参数区
 *
 * 参数指的是「现场要改、但代码不该重新编译」的那部分：通道表、轮询表、
 * 超时、MQTT 地址。以前这些都写在源码里，改一个从机地址要重新出固件。
 *
 * 存储用**双页交替**：两页各带序号，写入永远写「非当前有效」的那一页。
 * 这样任何时刻掉电，至少有一页是完整的：
 *
 *   写之前：[A: seq=7 有效][B: seq=6 旧]
 *   写 B（掉电在这里）→ A 仍然有效，B 是垃圾
 *   写完校验通过 → B 成为有效页，seq=8
 *
 * 单页原地覆盖的问题在于「写坏」比「不写」更糟：参数和 CRC 一起没了，
 * 设备只能回默认值，而默认值未必适合现场。
 *
 * 序列化不用结构体内存布局，逐字节显式编码。理由是跨编译器/跨平台的
 * 填充和对齐不一样，用 `memcpy(struct)` 存下来的东西换台机器读就是乱码，
 * 而且错得很安静 —— CRC 只会告诉你「对不上」，不会告诉你是哪个字段。
 */
#ifndef MG_PARAM_H
#define MG_PARAM_H

#include <stdint.h>

#include "mg_storage.h"
#include "mg_types.h"

#define MG_PARAM_PAGE_SIZE   512   /* 单页大小；两页共 1KB */
#define MG_PARAM_HDR_SIZE    16
#define MG_PARAM_PAYLOAD_MAX (MG_PARAM_PAGE_SIZE - MG_PARAM_HDR_SIZE)

#define MG_PARAM_MAX_CH     4
#define MG_PARAM_NAME_MAX   12
#define MG_PARAM_ARG_MAX    24
#define MG_PARAM_MAX_POLL   8
#define MG_PARAM_MQTT_MAX   40

typedef struct
{
    char name[MG_PARAM_NAME_MAX];
    uint8_t kind;  /* mg_chan_kind_t */
    char arg[MG_PARAM_ARG_MAX];
} mg_param_ch_t;

/*
 * 一条轮询项。网关按表轮转，一次发一条 ——
 * 同时发多条会让「哪条响应对应哪次请求」依赖 ID，而 Modbus RTU 的
 * 从机地址本身就是 ID，同地址的并发请求无法区分。
 */
typedef struct
{
    uint8_t ch;
    uint8_t slave;
    uint8_t func;
    uint16_t reg;
    uint16_t count; /* 连续读的寄存器个数 */
} mg_param_poll_t;

typedef struct
{
    uint8_t ch_count;
    mg_param_ch_t ch[MG_PARAM_MAX_CH];

    uint8_t poll_count;
    mg_param_poll_t poll[MG_PARAM_MAX_POLL];

    uint16_t poll_interval_ms;
    uint16_t session_timeout_ms;

    char mqtt_addr[MG_PARAM_MQTT_MAX];
    char mqtt_pub[MG_PARAM_MQTT_MAX];
    char mqtt_sub[MG_PARAM_MQTT_MAX];
} mg_param_t;

/* 出厂默认值：一路回环 Modbus、两个从机各四个寄存器、100ms 轮询 */
void mg_param_defaults(mg_param_t *p);

/*
 * 校验。字段越界、名字没结尾、轮询项引用了不存在的通道都会返回 -1。
 *
 * 校验单独成一个函数而不是塞进 load：从总线或串口灌进来的参数也必须过同一关，
 * 不能只在存储路径上检查。
 */
int mg_param_validate(const mg_param_t *p);

/*
 * 从存储加载。
 * 返回 0：读到有效页（*from_page 为 0 或 1）
 * 返回 1：两页都没有有效数据，out 填默认值 —— 首次上电就是这种
 * 返回 -1：后端读失败
 */
int mg_param_load(mg_param_t *out, const mg_storage_io_t *io,
                  const mg_param_t *def, int *from_page);

/*
 * 保存。写到非当前有效的页，写完回读校验。
 * 校验失败返回 -1，此时**旧页仍然有效**，设备不会因为一次保存失败变砖。
 */
int mg_param_save(const mg_param_t *p, const mg_storage_io_t *io);

/* 当前生效页号（0/1）；没有有效页时为 -1。供日志与测试观察 */
int mg_param_active_page(const mg_storage_io_t *io);

/* 显式编码/解码，暴露出来是为了让测试能构造「格式不对」的输入 */
int mg_param_encode(const mg_param_t *p, uint8_t *buf, uint16_t size, uint16_t *out_len);
int mg_param_decode(mg_param_t *p, const uint8_t *buf, uint16_t len);

#endif /* MG_PARAM_H */
