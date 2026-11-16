/*
 * mg_modbus.c —— Modbus RTU 组帧 / 校验 / 解析
 */
#include "mg_modbus.h"
#include <string.h>

uint16_t mg_mb_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    if (data == 0)
        return 0;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (uint8_t b = 0; b < 8; b++)
        {
            /* 逐位算法不用查表：网关固件对体积敏感，512 字节的表不值当 */
            if (crc & 0x0001)
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else
                crc >>= 1;
        }
    }

    return crc;
}

int mg_mb_build_read(uint8_t slave, uint8_t func, uint16_t reg,
                     uint8_t out[MG_FRAME_DATA])
{
    uint16_t crc;

    if (out == 0)
        return MG_ERR_PARAM;

    /* 广播地址 0 不用于读；248-255 为保留 */
    if (slave == 0 || slave > 247)
        return MG_ERR_PARAM;

    if (func != MB_FUNC_READ_COILS && func != MB_FUNC_READ_DISCRETE &&
        func != MB_FUNC_READ_HOLDING && func != MB_FUNC_READ_INPUT)
        return MG_ERR_PARAM;

    memset(out, 0, MG_FRAME_DATA);
    out[0] = slave;
    out[1] = func;
    out[2] = (uint8_t)(reg >> 8);
    out[3] = (uint8_t)(reg & 0xFF);
    out[4] = 0x00;
    out[5] = 0x01;              /* 固定读 1 个点，见头文件的说明 */

    crc = mg_mb_crc16(out, 6);
    out[6] = (uint8_t)(crc & 0xFF);   /* CRC 低字节在前 */
    out[7] = (uint8_t)(crc >> 8);

    return MG_OK;
}

int mg_mb_build_write_reg(uint8_t slave, uint16_t reg, uint16_t value,
                          uint8_t out[MG_FRAME_DATA])
{
    uint16_t crc;

    if (out == 0)
        return MG_ERR_PARAM;

    if (slave == 0 || slave > 247)
        return MG_ERR_PARAM;

    memset(out, 0, MG_FRAME_DATA);
    out[0] = slave;
    out[1] = MB_FUNC_WRITE_REGISTER;
    out[2] = (uint8_t)(reg >> 8);
    out[3] = (uint8_t)(reg & 0xFF);
    out[4] = (uint8_t)(value >> 8);
    out[5] = (uint8_t)(value & 0xFF);

    crc = mg_mb_crc16(out, 6);
    out[6] = (uint8_t)(crc & 0xFF);
    out[7] = (uint8_t)(crc >> 8);

    return MG_OK;
}

int mg_mb_parse_response(const uint8_t *buf, uint8_t len, uint8_t expect_slave,
                         uint8_t expect_func, uint16_t *out_value,
                         uint8_t *out_exc)
{
    uint16_t crc_calc, crc_recv;

    if (buf == 0 || len < 5)
        return MG_ERR_PARAM;

    crc_calc = mg_mb_crc16(buf, (uint16_t)(len - 2));
    crc_recv = (uint16_t)(buf[len - 2] | ((uint16_t)buf[len - 1] << 8));

    /*
     * CRC 必须校验。通道上任何一位翻转都会让解析出错误的寄存器值，
     * 而错误的保护阈值比没有阈值更危险 —— 它会让人以为设置了。
     */
    if (crc_calc != crc_recv)
        return MG_ERR_STATE;

    if (buf[0] != expect_slave)
        return MG_ERR_STATE;

    /* 异常响应：功能码 = 请求功能码 | 0x80 */
    if (buf[1] == (uint8_t)(expect_func | MB_EXC_FLAG))
    {
        if (out_exc != 0)
            *out_exc = buf[2];
        return MG_ERR_STATE;
    }

    if (buf[1] != expect_func)
        return MG_ERR_STATE;

    if (out_value != 0)
    {
        /* 读线圈/离散输入：字节数 + 位数据；读寄存器：字节数 + 2 字节值 */
        if (expect_func == MB_FUNC_READ_COILS || expect_func == MB_FUNC_READ_DISCRETE)
        {
            if (len < 6)
                return MG_ERR_STATE;
            *out_value = buf[3] & 0x01;
        }
        else
        {
            if (len < 7)
                return MG_ERR_STATE;
            *out_value = (uint16_t)((buf[3] << 8) | buf[4]);
        }
    }

    return MG_OK;
}
