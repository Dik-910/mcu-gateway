/*
 * mg_uplink_mqtt.c —— 基于 Paho C 的 MQTT 上行 + 下行
 *
 * 关于「有没有联调过」这句要诚实：
 * 本机没有 mosquitto（装不了），联调用的是 amqtt（独立的 Python broker 实现）。
 * 它不是 mosquitto，但同样是第三方实现、按 MQTT 3.1.1 说话，
 * 用它验证报文格式是有效的。CI 上会用 apt 装 mosquitto 再跑一遍。
 *
 * 三处容易踩的地方，代码里都标了：
 *   1. setCallbacks 必须在 connect 之前
 *   2. 回调里的 message 和 topicName 都要 free，否则每收一条泄漏一次
 *   3. 重连不能放在 publish 里顺手做 —— connect 可能阻塞数秒，
 *      会把调用 publish 的解析任务一起卡住
 */
#include "mg_uplink.h"
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
    opts.cleansession = 1;      /* 不要离线消息：设备侧都是最新值覆盖 */
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
    log_info("mqtt: connected %s, pub=%s sub=%s", g_addr, g_pub, g_sub);
    return 0;
}

static int up_open(void)
{
    if (getenv("MG_MQTT_ADDR") != 0)  g_addr = getenv("MG_MQTT_ADDR");
    if (getenv("MG_MQTT_PUB") != 0)   g_pub  = getenv("MG_MQTT_PUB");
    if (getenv("MG_MQTT_SUB") != 0)   g_sub  = getenv("MG_MQTT_SUB");
    if (getenv("MG_MQTT_CID") != 0)   g_cid  = getenv("MG_MQTT_CID");

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

static int up_publish(const char *json)
{
    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    if (json == 0)
        return -1;

    if (!g_connected)
    {
        g_pub_fail++;
        return -1;
    }

    msg.payload = (void *)json;
    msg.payloadlen = (int)strlen(json);
    msg.qos = 0;               /* 遥测用 QoS0：丢了下一周期还有 */
    msg.retained = 0;

    rc = MQTTClient_publishMessage(g_client, g_pub, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        /* 失败一次就标记断开，交给 tick 重连，不在这里重试 */
        g_connected = 0;
        g_pub_fail++;
        log_warn("mqtt: publish failed (%d)", rc);
        return -1;
    }

    g_pub_ok++;
    return 0;
}

static void up_tick(void)
{
    uint64_t now;

    if (g_connected)
        return;

    now = now_ms();
    if (now < g_retry_at_ms)
        return;

    if (do_connect() == 0)
    {
        g_backoff_ms = BACKOFF_MIN_MS;
        g_reconnects++;
        return;
    }

    g_backoff_ms = (g_backoff_ms * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : g_backoff_ms * 2;
    g_retry_at_ms = now + g_backoff_ms;

    log_info("mqtt: next retry in %u ms (pub_ok=%u pub_fail=%u reconn=%u)",
             (unsigned)g_backoff_ms, (unsigned)g_pub_ok,
             (unsigned)g_pub_fail, (unsigned)g_reconnects);
}

static void up_close(void)
{
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
