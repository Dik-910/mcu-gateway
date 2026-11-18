/*
 * mg_param_file.h —— 参数区的 PC 侧后端（文件模拟两页）
 */
#ifndef MG_PARAM_FILE_H
#define MG_PARAM_FILE_H

#include "mg_param.h"

/*
 * 从文件加载参数。文件不存在或两页都无效时填入默认值，
 * 并把默认值写回去 —— 这样「首次上电」和「参数丢失」都能自愈，
 * 不需要人工干预。
 *
 * from_page 返回实际生效的页号（-1 表示用的是默认值）。
 * 返回 MG_OK / MG_ERR_IO。
 */
int mg_param_load_file(const char *path, mg_param_t *p, int *from_page);

/* 保存到文件。返回 MG_OK / MG_ERR */
int mg_param_save_file(const char *path, const mg_param_t *p);

#endif /* MG_PARAM_FILE_H */
