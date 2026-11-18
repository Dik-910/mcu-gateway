/*
 * mg_outbox.h —— 掉电不丢的发送队列（outbox）
 *
 * 为什么需要这一层：
 *   MQTT 断线期间业务侧仍在校验报文、生成 JSON。直接丢掉的后果是
 *   「网关一直在跑，但云端那段时间什么都没有」——事后无法区分
 *   「设备没数据」和「数据被丢了」。
 *
 *   QoS1 只解决「交给 broker 之后至少到一次」，解决不了
 *   「本机还没交出去就掉电」。所以要先落盘，再谈投递。
 *
 * 语义（明确写死，不要猜）：
 *   - 入队成功即视为「已接受」，返回 0。**不代表已投递**。
 *   - 投递成功的判定是收到 PUBACK，由 mg_outbox_commit 显式确认。
 *   - 队列满时丢最旧的：遥测数据越旧越没价值，丢新的会让整段时间缺口。
 *   - 允许重复：提交前掉电，重放会重发已送达的那条。这是 at-least-once
 *     的固有代价，不是缺陷 —— 要「恰好一次」就得让消费端按业务键去重。
 *
 * 掉电一致性靠「状态字节最后写」：
 *   写一条记录的顺序是 失效(0) -> 长度/序号/CRC/数据 -> 有效(0xA5)。
 *   中途掉电时状态字节还是 0，这条记录不会被当成有效数据；
 *   而 CRC 又能挡住「状态字节还是上一轮残留的 0xA5」这种情况。
 *
 * 存储后端通过 io 回调注入：PC 上是文件（模拟 Flash 擦除态），
 * 目标板上就是 Flash 扇区。本层自己不碰任何平台 API。
 */
#ifndef MG_OUTBOX_H
#define MG_OUTBOX_H

#include <stdint.h>

#define MG_OUTBOX_PAYLOAD_MAX 200  /* 单条 JSON 上限，超过直接拒绝 */
#define MG_OUTBOX_SLOT_HDR    16   /* 状态1 + 长度1 + 保留2 + 序号4 + CRC2 + 保留6 */
#define MG_OUTBOX_SLOT_SIZE   (MG_OUTBOX_SLOT_HDR + MG_OUTBOX_PAYLOAD_MAX)
#define MG_OUTBOX_HDR_SIZE    16
#define MG_OUTBOX_SLOTS_MAX   256

/* 状态字节：只有 0xA5 表示「这条是完整的」 */
#define MG_OUTBOX_STATE_EMPTY 0x00u
#define MG_OUTBOX_STATE_VALID 0xA5u

/*
 * 后端读写。off 是扇区偏移，必须支持任意偏移、按字节读写 ——
 * 目标是让 Flash 驱动能直接对上（页内任意偏移可写，页擦除在这一层之上）。
 * 返回 0 表示成功，负值失败。
 */
typedef struct
{
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t n);
    int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t n);
    void *ctx;
} mg_outbox_io_t;

typedef struct
{
    uint32_t put;      /* 累计入队 */
    uint32_t sent;     /* 累计收到 PUBACK */
    uint32_t dropped;  /* 队列满被覆盖的最旧记录 */
    uint32_t rejected; /* 超长 / 后端写失败，没进队列 */
    uint32_t torn;     /* 初始化时丢弃的残缺记录 */
    uint32_t resync;   /* 头部不可用、从记录反推读取位置的次数 */
} mg_outbox_stats_t;

typedef struct
{
    mg_outbox_io_t io;
    uint32_t read_seq;  /* 下一条待投递的序号 */
    uint32_t write_seq; /* 下一次入队要用的序号 */
    uint16_t slots;
    uint8_t ready;
    mg_outbox_stats_t st;
} mg_outbox_t;

/*
 * 初始化并恢复。
 *
 * io 指向的后端必须已经可读写（文件要已打开，Flash 要不需再初始化）。
 * 返回 0 表示后端可用；-1 表示后端读不出来，此时 put/peek 都返回失败。
 * 后端是空白区（全 0 或全 0xFF）属于正常情况：按空队列处理。
 */
int mg_outbox_init(mg_outbox_t *ob, const mg_outbox_io_t *io, uint16_t slots);

/* 入队。成功返回 0；超长或写失败返回 -1 并计入 rejected */
int mg_outbox_put(mg_outbox_t *ob, const uint8_t *payload, uint16_t len);

/*
 * 取队首但不移除：返回 1 表示取到，0 表示空，-1 表示参数错。
 * seq 是这条记录的序号，提交时要原样传回 mg_outbox_commit。
 * buf 容量必须 >= MG_OUTBOX_PAYLOAD_MAX。
 */
int mg_outbox_peek(mg_outbox_t *ob, uint32_t *seq, uint8_t *buf, uint16_t *len);

/*
 * 确认已投递（收到 PUBACK 之后调）。
 *
 * 只接受队首序号：乱序确认会把中间那些还没发的记录静默跳过。
 * 传入已确认过的序号（重复确认）是无害的，直接返回 0。
 */
int mg_outbox_commit(mg_outbox_t *ob, uint32_t seq);

uint32_t mg_outbox_pending(const mg_outbox_t *ob);

#endif /* MG_OUTBOX_H */
