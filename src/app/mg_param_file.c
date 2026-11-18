/*
 * mg_param_file.c —— 参数区落在文件上（模拟两页 Flash）
 */
#include "mg_param_file.h"

#include <string.h>

#include "mg_outbox_file.h"
#include "mg_types.h"

#define PARAM_FILE_SIZE (2u * MG_PARAM_PAGE_SIZE)

static int open_param(mg_outbox_file_t *f, const char *path)
{
    return mg_outbox_file_open(f, path, PARAM_FILE_SIZE);
}

int mg_param_load_file(const char *path, mg_param_t *p, int *from_page)
{
    mg_outbox_file_t f;
    mg_storage_io_t io;
    mg_param_t def;
    int r;

    if (path == 0 || p == 0)
        return MG_ERR_IO;
    if (from_page != 0)
        *from_page = -1;

    mg_param_defaults(&def);

    if (open_param(&f, path) != 0)
    {
        /* 文件打不开（只读介质、权限）：用默认值继续跑，不因此罢工 */
        *p = def;
        return MG_OK;
    }

    mg_outbox_file_io(&f, &io);
    r = mg_param_load(p, &io, &def, from_page);

    if (r == 1)
    {
        /* 首次上电或两页都坏：把默认值落盘，下次启动就有据可依 */
        mg_param_save(p, &io);
    }

    mg_outbox_file_close(&f);
    return (r < 0) ? MG_ERR_IO : MG_OK;
}

int mg_param_save_file(const char *path, const mg_param_t *p)
{
    mg_outbox_file_t f;
    mg_storage_io_t io;
    int r;

    if (path == 0 || p == 0)
        return MG_ERR_IO;

    if (open_param(&f, path) != 0)
        return MG_ERR_IO;

    mg_outbox_file_io(&f, &io);
    r = mg_param_save(p, &io);
    mg_outbox_file_close(&f);

    return (r == 0) ? MG_OK : MG_ERR_IO;
}
