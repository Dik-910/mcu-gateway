/*
 * mg_modbus_test.c —— Modbus 组帧与解析
 *
 * 所有期望的字节串都不是用被测函数生成的，而是先用 Python
 * 独立实现算出来、再用文档中的已知样例（01 04 00 00 00 01 -> 31 CA）
 * 交叉核对过。否则就是「用被测代码验证被测代码」。
 */
#include <stdio.h>
#include <string.h>
#include "mg_modbus.h"

static int g_fail;
static int g_total;

static void check(int cond, const char *what)
{
    g_total++;
    if (!cond)
    {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void check_bytes(const uint8_t *got, const uint8_t *want, int n, const char *what)
{
    g_total++;
    if (memcmp(got, want, (size_t)n) != 0)
    {
        g_fail++;
        printf("  [FAIL] %s\n    got :", what);
        for (int i = 0; i < n; i++) printf(" %02X", got[i]);
        printf("\n    want:");
        for (int i = 0; i < n; i++) printf(" %02X", want[i]);
        printf("\n");
    }
}

static void test_crc(void)
{
    /*
     * 字节序易错点：本函数返回值的低字节才是线上先发的字节。
     * 文档里把 01 04 00 00 00 01 的 CRC 写作 0x31CA，
     * 线上顺序是 31 CA，所以函数返回 0xCA31。
     */
    static const uint8_t a[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t b[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t c[] = {0x02, 0x03, 0x00, 0x02, 0x00, 0x01};

    printf("\nCRC16/MODBUS\n");
    check(mg_mb_crc16(a, sizeof(a)) == 0xCA31, "known sample 01 04 00 00 00 01 -> 31 CA");
    check(mg_mb_crc16(b, sizeof(b)) == 0x0A84, "01 03 00 00 00 01 -> 84 0A");
    check(mg_mb_crc16(c, sizeof(c)) == 0xF925, "02 03 00 02 00 01 -> 25 F9");
    check(mg_mb_crc16(0, 0) == 0, "null input safe");
}

static void test_build(void)
{
    uint8_t f[MG_FRAME_DATA];
    static const uint8_t want_read0[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A};
    static const uint8_t want_read2[] = {0x02, 0x03, 0x00, 0x02, 0x00, 0x01, 0x25, 0xF9};

    printf("\nBuild request\n");
    check(mg_mb_build_read(1, MB_FUNC_READ_HOLDING, 0, f) == MG_OK, "build read ok");
    check_bytes(f, want_read0, 8, "read slave1 reg0 byte-exact");

    check(mg_mb_build_read(2, MB_FUNC_READ_HOLDING, 2, f) == MG_OK, "build read ok (slave2)");
    check_bytes(f, want_read2, 8, "read slave2 reg2 byte-exact");

    check(mg_mb_build_read(0, MB_FUNC_READ_HOLDING, 0, f) == MG_ERR_PARAM,
          "slave 0 rejected (broadcast not readable)");
    check(mg_mb_build_read(248, MB_FUNC_READ_HOLDING, 0, f) == MG_ERR_PARAM,
          "slave 248 rejected (reserved)");
    check(mg_mb_build_read(1, 0x07, 0, f) == MG_ERR_PARAM, "unknown function rejected");

    /* 写单寄存器：slave1 reg2 = 0x003C */
    check(mg_mb_build_write_reg(1, 2, 0x003C, f) == MG_OK, "build write ok");
    check(f[0] == 0x01 && f[1] == 0x06 && f[4] == 0x00 && f[5] == 0x3C,
          "write frame layout");
    {
        uint16_t crc = mg_mb_crc16(f, 6);
        check(f[6] == (crc & 0xFF) && f[7] == (crc >> 8), "write crc matches");
    }
}

static void test_parse(void)
{
    uint8_t frame[8];
    uint16_t val = 0;
    uint8_t exc = 0;
    uint16_t crc;

    printf("\nParse response\n");

    /* 正常响应：01 03 02 00 C8 + CRC，值应为 200 */
    frame[0] = 0x01; frame[1] = 0x03; frame[2] = 0x02;
    frame[3] = 0x00; frame[4] = 0xC8;
    crc = mg_mb_crc16(frame, 5);
    frame[5] = (uint8_t)(crc & 0xFF);
    frame[6] = (uint8_t)(crc >> 8);

    check(mg_mb_parse_response(frame, 7, 1, MB_FUNC_READ_HOLDING, &val, &exc) == MG_OK,
          "valid response accepted");
    check(val == 200, "value decoded as 200");
    check(frame[5] == 0xB9 && frame[6] == 0xD2, "response crc byte-exact (B9 D2)");

    /* CRC 错：改动数据字节 */
    frame[4] ^= 0x01;
    val = 0xFFFF;
    check(mg_mb_parse_response(frame, 7, 1, MB_FUNC_READ_HOLDING, &val, &exc) != MG_OK,
          "corrupted payload rejected by crc");
    check(val == 0xFFFF, "value untouched when crc fails");
    frame[4] ^= 0x01;

    /* 从机地址不匹配 */
    check(mg_mb_parse_response(frame, 7, 2, MB_FUNC_READ_HOLDING, &val, &exc) != MG_OK,
          "wrong slave rejected");

    /* 功能码不匹配 */
    check(mg_mb_parse_response(frame, 7, 1, MB_FUNC_READ_INPUT, &val, &exc) != MG_OK,
          "wrong function rejected");

    /* 异常响应：01 83 02（非法数据地址） */
    frame[0] = 0x01; frame[1] = 0x83; frame[2] = 0x02;
    crc = mg_mb_crc16(frame, 3);
    frame[3] = (uint8_t)(crc & 0xFF);
    frame[4] = (uint8_t)(crc >> 8);

    exc = 0;
    check(mg_mb_parse_response(frame, 5, 1, MB_FUNC_READ_HOLDING, &val, &exc) != MG_OK,
          "exception response reported as failure");
    check(exc == 0x02, "exception code extracted");

    /* 长度不足 */
    check(mg_mb_parse_response(frame, 3, 1, MB_FUNC_READ_HOLDING, &val, &exc) != MG_OK,
          "short frame rejected");
    check(mg_mb_parse_response(0, 8, 1, MB_FUNC_READ_HOLDING, &val, &exc) == MG_ERR_PARAM,
          "null buffer rejected");
}

static void test_coils(void)
{
    uint8_t frame[6];
    uint16_t val = 0;
    uint8_t exc = 0;
    uint16_t crc;

    printf("\nParse coil response\n");

    /* 读线圈响应：01 01 01 01 + CRC -> 值为 1 */
    frame[0] = 0x01; frame[1] = 0x01; frame[2] = 0x01; frame[3] = 0x01;
    crc = mg_mb_crc16(frame, 4);
    frame[4] = (uint8_t)(crc & 0xFF);
    frame[5] = (uint8_t)(crc >> 8);

    check(mg_mb_parse_response(frame, 6, 1, MB_FUNC_READ_COILS, &val, &exc) == MG_OK,
          "coil response accepted");
    check(val == 1, "coil bit decoded");
}

int main(void)
{
    printf("=== mg_modbus_test ===\n");

    test_crc();
    test_build();
    test_parse();
    test_coils();

    printf("\nResult: %d checks, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
