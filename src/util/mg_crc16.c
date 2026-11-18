/*
 * mg_crc16.c —— CRC-16/CCITT-FALSE
 */
#include "mg_crc16.h"

uint16_t mg_crc16_update(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    return crc;
}

uint16_t mg_crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFFu;

    for (uint16_t i = 0; i < n; i++)
        crc = mg_crc16_update(crc, p[i]);
    return crc;
}
