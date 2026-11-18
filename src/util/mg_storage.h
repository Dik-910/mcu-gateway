/*
 * mg_storage.h —— 存储后端接口
 *
 * outbox 和参数区共用同一个抽象：按偏移读写、按字节粒度。
 *
 * 为什么只要这两个动作：目标板上是 Flash，它有「擦除」这个额外动作，
 * 但擦除的粒度是一页，属于「怎么用这段区域」的问题，分别由 outbox
 * （环形，擦除即覆盖写）和参数区（双页交替，写另一页）各自处理。
 * 把擦除放进这层会强迫文件后端实现一个假的擦除动作。
 */
#ifndef MG_STORAGE_H
#define MG_STORAGE_H

#include <stdint.h>

typedef struct
{
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t n);
    int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t n);
    void *ctx;
} mg_storage_io_t;

#endif /* MG_STORAGE_H */
