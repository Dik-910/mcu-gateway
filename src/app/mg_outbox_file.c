/*
 * mg_outbox_file.c —— 用文件模拟 Flash 扇区
 *
 * 只做偏移读写，不做缓冲：outbox 已经保证了写入顺序，
 * 再加一层缓冲反而会在掉电时把「已提交」变成「还在内存里」。
 */
#include "mg_outbox_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    mg_outbox_file_t *f = (mg_outbox_file_t *)ctx;
    ssize_t r = pread(f->fd, buf, (size_t)n, (off_t)off);

    return (r == (ssize_t)n) ? 0 : -1;
}

static int file_write(void *ctx, uint32_t off, const void *buf, uint32_t n)
{
    mg_outbox_file_t *f = (mg_outbox_file_t *)ctx;
    ssize_t r = pwrite(f->fd, buf, (size_t)n, (off_t)off);

    /*
     * 不在这里 fsync：每条消息都 fsync 会让 Flash 寿命和吞吐都难堪。
     * 真掉电时丢的是「最后几条还没落盘的」，这正是 outbox 允许的范围
     * ——比内存队列好，但别把它当数据库用。
     */
    return (r == (ssize_t)n) ? 0 : -1;
}

int mg_outbox_file_open(mg_outbox_file_t *f, const char *path, uint32_t size)
{
    struct stat st;

    if (f == 0 || path == 0 || size == 0)
        return -1;

    f->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (f->fd < 0)
        return -1;

    if (fstat(f->fd, &st) != 0)
    {
        close(f->fd);
        f->fd = -1;
        return -1;
    }

    if ((uint32_t)st.st_size < size)
    {
        if (ftruncate(f->fd, (off_t)size) != 0)
        {
            close(f->fd);
            f->fd = -1;
            return -1;
        }
    }

    return 0;
}

void mg_outbox_file_close(mg_outbox_file_t *f)
{
    if (f != 0 && f->fd >= 0)
    {
        close(f->fd);
        f->fd = -1;
    }
}

void mg_outbox_file_io(mg_outbox_file_t *f, mg_outbox_io_t *io)
{
    if (io == 0)
        return;

    io->read = file_read;
    io->write = file_write;
    io->ctx = f;
}
