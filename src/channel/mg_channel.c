/*
 * mg_channel.c —— 通道通用包装
 *
 * 这一层只做两件事：参数校验、统计计数。
 * 看起来多余，但把校验集中在这里，各平台的实现就不必重复检查 ——
 * 否则会出现「CAN 版检查了 dlc，串口版忘了检查」的分歧，
 * 而这种分歧只在上线后才暴露。
 */
#include "mg_channel.h"
#include <stdio.h>
#include <string.h>
#include "log.h"

int mg_channel_open(mg_channel_t *ch, const char *name, mg_chan_kind_t kind,
                    const mg_channel_ops_t *ops, const char *arg)
{
    if (ch == 0 || ops == 0 || ops->recv == 0 || arg == 0)
        return MG_ERR_PARAM;

    memset(ch, 0, sizeof(*ch));
    ch->kind = kind;
    ch->ops = ops;

    if (name != 0)
        snprintf(ch->name, sizeof(ch->name), "%s", name);

    if (ops->open == 0 || ops->open(ch, arg) != 0)
        return MG_ERR_IO;

    log_info("channel %s opened (kind=%s)", ch->name,
             kind == MG_KIND_CAN ? "can" : "modbus");
    return MG_OK;
}

/*
 * 收一帧。
 *
 * dlc 越界在这里拦掉：驱动实现可能因为寄存器异常返回脏数据，
 * 让越界的 dlc 流到解析层，会造成缓冲区越读 —— 在 MCU 上直接 HardFault。
 */
int mg_channel_recv(mg_channel_t *ch, mg_frame_t *f)
{
    int rc;

    if (ch == 0 || f == 0 || ch->ops == 0)
        return MG_ERR_PARAM;

    rc = ch->ops->recv(ch, f);

    if (rc == MG_OK)
    {
        if (f->dlc > MG_FRAME_DATA)
        {
            ch->rx_errors++;
            return MG_ERR_PARAM;
        }
        ch->rx_frames++;
    }
    else if (rc != MG_ERR_NOMSG)
    {
        /* NOMSG 是正常状态（队列空），不计入错误 */
        ch->rx_errors++;
    }

    return rc;
}

int mg_channel_send(mg_channel_t *ch, const mg_frame_t *f)
{
    if (ch == 0 || f == 0 || ch->ops == 0 || ch->ops->send == 0)
        return MG_ERR_PARAM;

    if (f->dlc > MG_FRAME_DATA)
        return MG_ERR_PARAM;

    if (ch->ops->send(ch, f) != 0)
    {
        ch->tx_errors++;
        return MG_ERR_IO;
    }

    ch->tx_frames++;
    return MG_OK;
}

void mg_channel_close(mg_channel_t *ch)
{
    if (ch == 0 || ch->ops == 0)
        return;

    if (ch->ops->close != 0)
        ch->ops->close(ch);
}
