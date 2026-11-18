/*
 * mg_crc16.h —— CRC-16/CCITT-FALSE，本机存储的唯一实现
 *
 * outbox 与参数区都用它校验存下来的字节。两处各写一份的实现迟早会分叉，
 * 而分叉之后「同样的数据一处能读、一处读不出」，排查起来非常费时间。
 *
 * 注意它和 Modbus 的 CRC 不是一回事（多项式 0x1021 vs 0xA001，位序也不同），
 * 总线报文的 CRC 仍在 mg_modbus.c 里，两者服务对象不同，不该合并。
 */
#ifndef MG_CRC16_H
#define MG_CRC16_H

#include <stdint.h>

/* 初值 0xFFFF，可用于分段累计 */
uint16_t mg_crc16_update(uint16_t crc, uint8_t b);

uint16_t mg_crc16(const uint8_t *p, uint16_t n);

#endif /* MG_CRC16_H */
