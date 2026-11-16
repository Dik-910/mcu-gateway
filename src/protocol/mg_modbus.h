/*
 * mg_modbus.h —— Modbus RTU 组帧与解析
 *
 * 说明一个前提：这里只实现「单寄存器 / 单线圈」的读写。
 *
 * 原因很实际：Modbus 响应帧最长可达 256 字节，而 CAN 单帧只有 8 字节。
 * 原课程项目是把 Modbus 直接塞进 CAN 的（ID 高 8 位装从机地址、
 * 低 8 位装功能码），能成立的前提就是「一次只读一个点」。
 * 一旦要读多个寄存器，响应必然超过 8 字节，这套映射立刻失效。
 *
 * 所以这里把限制写在接口上：count 只能是 1。
 * 要读多个点，就发多次请求 —— 这比「看起来支持批量、实际会截断」安全。
 */
#ifndef MG_MODBUS_H
#define MG_MODBUS_H

#include <stdint.h>
#include "mg_types.h"

/* 功能码 */
#define MB_FUNC_READ_COILS        0x01
#define MB_FUNC_READ_DISCRETE     0x02
#define MB_FUNC_READ_HOLDING      0x03
#define MB_FUNC_READ_INPUT        0x04
#define MB_FUNC_WRITE_COIL        0x05
#define MB_FUNC_WRITE_REGISTER    0x06

/* 异常响应：功能码最高位置 1 */
#define MB_EXC_FLAG               0x80

/* CRC16/MODBUS：初值 0xFFFF，反射多项式 0xA001 */
uint16_t mg_mb_crc16(const uint8_t *data, uint16_t len);

/*
 * 组一帧读请求。输出固定 8 字节，正好是一帧 CAN 的数据长度。
 * 返回 MG_OK 或 MG_ERR_PARAM（参数越界）。
 */
int mg_mb_build_read(uint8_t slave, uint8_t func, uint16_t reg,
                     uint8_t out[MG_FRAME_DATA]);

/* 组一帧写单寄存器请求（同样 8 字节） */
int mg_mb_build_write_reg(uint8_t slave, uint16_t reg, uint16_t value,
                          uint8_t out[MG_FRAME_DATA]);

/*
 * 校验并解析响应。
 * 返回 MG_OK 时把数据写入 out_value / out_len；
 * 如果是对端回的异常响应，返回 MG_ERR_STATE 并把异常码写入 out_exc。
 */
int mg_mb_parse_response(const uint8_t *buf, uint8_t len, uint8_t expect_slave,
                         uint8_t expect_func, uint16_t *out_value,
                         uint8_t *out_exc);

#endif /* MG_MODBUS_H */
