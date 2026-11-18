/*
 * mg_uplink_mqtt.c —— 基于 Paho C 的 MQTT 上行 + 下行
 *
 * 投递语义：
 *   上行 QoS1，且消息先落盘（mg_outbox）再发。所以 broker 掉线、
 *   网关自己重启，未投递的数据都还在队列里，连上以后按原顺序补发。
 *   判定「已投递」的唯一依据是收到 PUBACK；在此之前记录一直留在盘上。
 *
 *   代价是 at-least-once：PUBACK 丢失时会重发同一条。要做到「恰好一次」
 *   只能靠消费端按业务键去重，协议层给不了。
 *
 *   会话是持久的（cleansession=0，client_id 固定）：broker 侧保留订阅关系，
 *   网关重启后不必依赖自己重新订阅成功才开始收下行。
 *
 * 三处容易踩的地方，代码里都标了：
 *   1. setCallbacks 必须在 connect 之前
 *   2. 回调里的 message 和 topicName 都要 free，否则每收一条泄漏一次
 *   3. 重连和补发都不能放在 publish 里顺手做 —— connect 可能阻塞数秒，
 *      waitForCompletion 也要等 PUBACK，都会把调用 publish 的解析任务卡住
 */
#include "mg_uplink.h"
#include "mg_outbox.h"
#include "mg_outbox_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"

/* ---------- 下行环形缓冲（所有实现共用） ---------- */

#define DL_DEPTH 4

static char g_dl[DL_DEPTH][MG_DOWNLINK_MAX];
static volatile uint32_t g_dl_head;
static volatile uint32_t g_dl_tail;
static volatile uint32_t g_dl_dropped;

int mg_uplink_push_cmd(const char *json)
{
    size_t n;

    if (json == 0)
        return -1;

    n = strlen(json);
    if (n == 0 || n >= MG_DOWNLINK_MAX)
        return -1;

    /* 单生产者：只有 MQTT 回调线程写 head。满则丢，不阻塞回调 */
    if (g_dl_head - g_dl_tail >= DL_DEPTH)
    {
        g_dl_dropped++;
        return -1;
    }

    memcpy(g_dl[g_dl_head % DL_DEPTH], json, n);
    g_dl[g_dl_head % DL_DEPTH][n] = '\0';
    g_dl_head++;

    return 0;
}

int mg_uplink_poll_cmd(char *buf, int len)
{
    const char *s;
    int n;

    if (buf == 0 || len <= 1)
        return 0;

    /* 单消费者：只有 cmd_task 写 tail */
    if (g_dl_tail == g_dl_head)
        return 0;

    s = g_dl[g_dl_tail % DL_DEPTH];
    n = (int)strlen(s);

    if (n >= len)
        n = len - 1;

    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    g_dl_tail++;

    return n;
}

#ifdef MG_HAVE_MQTT

#include <MQTTClient.h>

#define BACKOFF_MIN_MS  1000
#define BACKOFF_MAX_MS  30000

/*
 * 单次 tick 最多补发几条。comm_task 周期 100ms，所以上限约 80 条/秒 ——
 * 高于采集速率，积压时能追上，又不会让一个 tick 把别的活全占了。
 */
#define DRAIN_BUDGET     8
/* 单条等 PUBACK 的上限。超时按失败处理，下个 tick 重发（允许重复） */
#define DRAIN_ACK_MS     300

/*
 * 队列容量决定「断线能扛多久」：128 格 × 216B ≈ 27KB。
 * 本机采集约 10 条/秒，所以能覆盖约 13 秒的断线；
 * 采样更慢的现场能覆盖更久。超过这个窗口就丢最旧，并由 dropped 计数暴露。
 */
#define OUTBOX_SLOTS     128

static MQTTClient g_client;
static int g_client_created;
static volatile int g_connected;

static const char *g_addr  = "tcp://127.0.0.1:1883";
static const char *g_pub   = "gw/uplink";
static const char *g_sub   = "gw/downlink";
static const char *g_cid   = "mcu-gateway";

static uint32_t g_backoff_ms = BACKOFF_MIN_MS;
static uint64_t g_retry_at_ms;

static uint32_t g_pub_ok, g_pub_fail, g_reconnects;

static mg_outbox_file_t g_ob_file;
static mg_outbox_t g_ob;
static int g_ob_ready;

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---------- Paho 回调 ---------- */

static void on_connlost(void *ctx, char *cause)
{
    (void)ctx;
    g_connected = 0;
    log_warn("mqtt: connection lost (%s)", cause ? cause : "?");
}

static int on_message(void *ctx, char *topic, int topic_len,
                      MQTTClient_message *msg)
{
    (void)ctx;
    (void)topic_len;

    if (msg != 0 && msg->payloadlen > 0)
    {
        /* 回调里不做解析、不调 RTOS API，只搬进环形缓冲 */
        {
            char tmp[MG_DOWNLINK_MAX];
            int n = msg->payloadlen;

            if (n >= MG_DOWNLINK_MAX)
                n = MG_DOWNLINK_MAX - 1;

            memcpy(tmp, msg->payload, (size_t)n);
            tmp[n] = '\0';
            mg_uplink_push_cmd(tmp);
        }
        log_info("mqtt: downlink %d bytes on %s", msg->payloadlen, topic ? topic : "?");
    }

    /*
     * 两个都要释放。少放任何一个，每收一条消息泄漏一次 ——
     * 在 PC 上看不出来，跑几小时才发现 RSS 一直涨。
     */
    if (msg != 0)
        MQTTClient_freeMessage(&msg);
    if (topic != 0)
        MQTTClient_free(topic);

    return 1;
}

static void on_delivered(void *ctx, MQTTClient_deliveryToken dt)
{
    (void)ctx;
    (void)dt;
}

/* ---------- 连接 ---------- */

static int do_connect(void)
{
    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    int rc;

    if (!g_client_created)
    {
        rc = MQTTClient_create(&g_client, g_addr, g_cid,
                               MQTTCLIENT_PERSISTENCE_NONE, NULL);
        if (rc != MQTTCLIENT_SUCCESS)
        {
            log_error("mqtt: create failed (%d)", rc);
            return -1;
        }
        g_client_created = 1;

        /* 必须在 connect 之前设置，否则收不到下行 */
        rc = MQTTClient_setCallbacks(g_client, NULL, on_connlost,
                                     on_message, on_delivered);
        if (rc != MQTTCLIENT_SUCCESS)
        {
            log_error("mqtt: setCallbacks failed (%d)", rc);
            MQTTClient_destroy(&g_client);
            g_client_created = 0;
            return -1;
        }
    }

    opts.keepAliveInterval = 20;
    /*
     * 持久会话。
     *
     * 原来用 cleansession=1 的理由是「设备侧都是最新值覆盖，不需要离线消息」，
     * 那是针对**下行**说的：云端发的是写指令，重连后补一条旧指令没有意义。
     * 但上行不同 —— 断线期间采到的数据已经进了 outbox，需要按顺序补发；
     * 若每次连接都清会话，broker 侧连订阅关系一起丢，
     * 补发之后第一条下行指令仍可能落在重订阅的空窗里。
     */
    opts.cleansession = 0;
    opts.connectTimeout = 3;

    rc = MQTTClient_connect(g_client, &opts);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        log_warn("mqtt: connect %s failed (%d), retry in %u ms",
                 g_addr, rc, (unsigned)g_backoff_ms);
        return -1;
    }

    rc = MQTTClient_subscribe(g_client, g_sub, 1);
    if (rc != MQTTCLIENT_SUCCESS)
        log_warn("mqtt: subscribe %s failed (%d)", g_sub, rc);

    g_connected = 1;
    log_info("mqtt: connected %s, pub=%s sub=%s, pending=%u",
             g_addr, g_pub, g_sub, (unsigned)mg_outbox_pending(&g_ob));
    return 0;
}

static int up_open(void)
{
    const char *path;

    if (getenv("MG_MQTT_ADDR") != 0)  g_addr = getenv("MG_MQTT_ADDR");
    if (getenv("MG_MQTT_PUB") != 0)   g_pub  = getenv("MG_MQTT_PUB");
    if (getenv("MG_MQTT_SUB") != 0)   g_sub  = getenv("MG_MQTT_SUB");
    if (getenv("MG_MQTT_CID") != 0)   g_cid  = getenv("MG_MQTT_CID");

    /*
     * 落盘队列。路径可用 MG_OUTBOX_FILE 覆盖，默认当前目录下的文件。
     * 目标板上这里换成 Flash 扇区，其余代码不用改。
     */
    path = getenv("MG_OUTBOX_FILE");
    if (path == 0)
        path = "gw_outbox.bin";

    if (mg_outbox_file_open(&g_ob_file, path,
                            MG_OUTBOX_HDR_SIZE + OUTBOX_SLOTS * MG_OUTBOX_SLOT_SIZE) == 0)
    {
        mg_outbox_io_t io;

        mg_outbox_file_io(&g_ob_file, &io);
        if (mg_outbox_init(&g_ob, &io, OUTBOX_SLOTS) == 0)
        {
            g_ob_ready = 1;
            log_info("outbox: %s, pending=%u", path, (unsigned)mg_outbox_pending(&g_ob));
        }
        else
        {
            log_error("outbox: init failed on %s, uplink falls back to best effort", path);
            mg_outbox_file_close(&g_ob_file);
        }
    }
    else
    {
        log_error("outbox: cannot open %s, uplink falls back to best effort", path);
    }

    if (do_connect() == 0)
    {
        g_backoff_ms = BACKOFF_MIN_MS;
        return 0;
    }

    /* 首次连不上不算致命：tick 会按退避重试 */
    g_retry_at_ms = now_ms() + g_backoff_ms;
    g_backoff_ms = (g_backoff_ms * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : g_backoff_ms * 2;
    return -1;
}

/*
 * 发一条并等 PUBACK。
 *
 * 同步等确认是刻意的：只有拿到确认才能从盘上删掉，否则「发了」和
 * 「对方收到了」没法区分。这一步阻塞，所以只在 comm_task 里做。
 */
static int publish_one(const uint8_t *payload, uint16_t len)
{
    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    msg.payload = (void *)payload;
    msg.payloadlen = (int)len;
    msg.qos = 1;
    msg.retained = 0;

    rc = MQTTClient_publishMessage(g_client, g_pub, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS)
        return -1;

    rc = MQTTClient_waitForCompletion(g_client, token, DRAIN_ACK_MS);
    if (rc != MQTTCLIENT_SUCCESS)
        return -1;

    return 0;
}

/* 按序补发。budget 限制本次最多动几条，避免长时间占着 comm_task */
static void outbox_drain(int budget)
{
    uint8_t buf[MG_OUTBOX_PAYLOAD_MAX];

    if (!g_connected || !g_ob_ready)
        return;

    for (int i = 0; i < budget; i++)
    {
        uint32_t seq;
        uint16_t len;
        int r = mg_outbox_peek(&g_ob, &seq, buf, &len);

        if (r <= 0)
            break;

        if (publish_one(buf, len) != 0)
        {
            /*
             * 不在这里无限重试：把连接标记成断开，交给 tick 走退避重连。
             * 记录还留在盘上，重连后会再发一次 —— 可能重复，但不会丢。
             */
            g_pub_fail++;
            g_connected = 0;
            log_warn("mqtt: publish failed, %u record(s) pending",
                     (unsigned)mg_outbox_pending(&g_ob));
            break;
        }

        mg_outbox_commit(&g_ob, seq);
        g_pub_ok++;
    }
}

static int up_publish(const char *json)
{
    uint16_t len;

    if (json == 0)
        return -1;

    /*
     * 只有入队成功才算「已接受」。落盘之后即使马上掉电也不会丢，
     * 所以这里返回 0 表示「网关接下了」，不代表 broker 已经收到。
     */
    if (!g_ob_ready)
    {
        /* 没有落盘后端时退化成尽力而为，直接发，不排队 */
        if (!g_connected)
        {
            g_pub_fail++;
            return -1;
        }
        return publish_one((const uint8_t *)json, (uint16_t)strlen(json));
    }

    len = (uint16_t)strlen(json);
    if (mg_outbox_put(&g_ob, (const uint8_t *)json, len) != 0)
        return -1;

    return 0;
}

static void up_tick(void)
{
    uint64_t now;

    if (g_connected)
    {
        outbox_drain(DRAIN_BUDGET);
        return;
    }

    now = now_ms();
    if (now < g_retry_at_ms)
        return;

    if (do_connect() == 0)
    {
        g_backoff_ms = BACKOFF_MIN_MS;
        g_reconnects++;

        /* 刚连上就补发：断线期间攒下的数据不能等下一个业务周期 */
        outbox_drain(DRAIN_BUDGET);
        return;
    }

    g_backoff_ms = (g_backoff_ms * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : g_backoff_ms * 2;
    g_retry_at_ms = now + g_backoff_ms;

    log_info("mqtt: next retry in %u ms (pub_ok=%u pub_fail=%u reconn=%u "
             "pending=%u put=%u sent=%u dropped=%u rejected=%u)",
             (unsigned)g_backoff_ms, (unsigned)g_pub_ok,
             (unsigned)g_pub_fail, (unsigned)g_reconnects,
             (unsigned)mg_outbox_pending(&g_ob), (unsigned)g_ob.st.put,
             (unsigned)g_ob.st.sent, (unsigned)g_ob.st.dropped,
             (unsigned)g_ob.st.rejected);
}

static void up_close(void)
{
    if (g_ob_ready)
    {
        log_info("outbox: closing, pending=%u dropped=%u",
                 (unsigned)mg_outbox_pending(&g_ob), (unsigned)g_ob.st.dropped);
        mg_outbox_file_close(&g_ob_file);
        g_ob_ready = 0;
    }

    if (g_client_created)
    {
        if (g_connected)
            MQTTClient_disconnect(g_client, 1000);
        MQTTClient_destroy(&g_client);
        g_client_created = 0;
        g_connected = 0;
    }
}

#else /* !MG_HAVE_MQTT */

static int up_open(void)
{
    log_warn("mqtt: built without Paho, uplink disabled");
    return -1;
}

static int up_publish(const char *json)
{
    (void)json;
    return -1;
}

static void up_tick(void)
{
}

static void up_close(void)
{
}

#endif /* MG_HAVE_MQTT */

static const mg_uplink_ops_t g_mqtt_ops =
{
    up_open, up_publish, up_tick, up_close
};

const mg_uplink_ops_t *mg_uplink_mqtt_ops(void)
{
    return &g_mqtt_ops;
}
