/*
 * mg_chan_socketcan.c —— Linux SocketCAN 通道
 *
 * 这是「PC 上可验证」这一目标的关键实现：
 * 上层任务、队列、会话逻辑全都不变，只把通道实现从回环换成 SocketCAN，
 * 就能在真实的 CAN 子系统和 vcan 上跑同一套代码。
 *
 * 到 STM32 上，换掉这个文件即可，上面一层一行都不用改。
 */
#define _DEFAULT_SOURCE

#include "mg_channel.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include "log.h"

typedef struct
{
    int fd;
} sc_ctx_t;

static sc_ctx_t g_sc[MG_CHANNEL_MAX];

static int sc_open(mg_channel_t *ch, const char *arg)
{
    int fd;
    struct ifreq ifr;
    struct sockaddr_can addr;
    int loopback = 0;

    if (ch->index >= MG_CHANNEL_MAX)
        return -1;

    fd = socket(AF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0)
    {
        log_error("socketcan: socket() failed: %s", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", arg);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        log_error("socketcan: interface %s not found: %s", arg, strerror(errno));
        close(fd);
        return -1;
    }

    /*
     * 关闭本地回环：网关自己发出去的帧不该再被自己当成外来数据收一遍。
     * vcan 默认会回环，不关的话转发链路会把帧越放越多。
     */
    setsockopt(fd, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &loopback, sizeof(loopback));

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        log_error("socketcan: bind(%s) failed: %s", arg, strerror(errno));
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    {
        log_error("socketcan: set nonblock failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    g_sc[ch->index].fd = fd;
    ch->ctx = &g_sc[ch->index];
    return 0;
}

static int sc_recv(mg_channel_t *ch, mg_frame_t *f)
{
    sc_ctx_t *c = (sc_ctx_t *)ch->ctx;
    struct can_frame cf;
    ssize_t n;

    if (c == 0 || c->fd < 0)
        return MG_ERR_STATE;

    n = read(c->fd, &cf, sizeof(cf));

    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MG_ERR_NOMSG : MG_ERR_IO;

    if (n != (ssize_t)sizeof(cf))
        return MG_ERR_IO;

    /* 错误帧不是数据，交给上层统计，不进解析 */
    if (cf.can_id & CAN_ERR_FLAG)
        return MG_ERR_IO;

    memset(f, 0, sizeof(*f));
    f->id = cf.can_id & CAN_SFF_MASK;
    f->dlc = (cf.can_dlc > MG_FRAME_DATA) ? MG_FRAME_DATA : cf.can_dlc;
    f->channel = ch->index;
    memcpy(f->data, cf.data, f->dlc);
    return MG_OK;
}

static int sc_send(mg_channel_t *ch, const mg_frame_t *f)
{
    sc_ctx_t *c = (sc_ctx_t *)ch->ctx;
    struct can_frame cf;
    ssize_t n;

    if (c == 0 || c->fd < 0)
        return -1;

    memset(&cf, 0, sizeof(cf));
    cf.can_id = f->id & CAN_SFF_MASK;
    cf.can_dlc = (f->dlc > MG_FRAME_DATA) ? MG_FRAME_DATA : f->dlc;
    memcpy(cf.data, f->data, cf.can_dlc);

    n = write(c->fd, &cf, sizeof(cf));
    if (n != (ssize_t)sizeof(cf))
    {
        /* 发送失败不重试：总线已经满了，重试只会加重拥塞 */
        return -1;
    }

    return 0;
}

static void sc_close(mg_channel_t *ch)
{
    sc_ctx_t *c = (sc_ctx_t *)ch->ctx;

    if (c != 0 && c->fd >= 0)
    {
        close(c->fd);
        c->fd = -1;
    }
    ch->ctx = 0;
}

static const mg_channel_ops_t g_sc_ops =
{
    sc_open, sc_recv, sc_send, sc_close
};

const mg_channel_ops_t *mg_chan_socketcan_ops(void)
{
    return &g_sc_ops;
}
