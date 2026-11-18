/*
 * mg_outbox_file.h —— outbox 的 PC 侧后端（文件模拟 Flash 扇区）
 *
 * 目标板上换成 Flash 驱动时，只需要另写一个 read/write 实现注入
 * mg_outbox_init，本文件不参与编译即可。
 */
#ifndef MG_OUTBOX_FILE_H
#define MG_OUTBOX_FILE_H

#include <stdint.h>

#include "mg_outbox.h"

typedef struct
{
    int fd;
} mg_outbox_file_t;

/*
 * 打开（必要时创建）后端文件，并保证长度不小于 size。
 * 新建的文件是全 0，对 outbox 来说等于空扇区，不需要额外擦除。
 * 返回 0 成功，-1 失败。
 */
int mg_outbox_file_open(mg_outbox_file_t *f, const char *path, uint32_t size);

void mg_outbox_file_close(mg_outbox_file_t *f);

/* 填好 io 结构，供 mg_outbox_init 使用 */
void mg_outbox_file_io(mg_outbox_file_t *f, mg_outbox_io_t *io);

#endif /* MG_OUTBOX_FILE_H */
