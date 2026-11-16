/*
 * mg_uplink_mqtt.c —— 基于 Paho C 的 MQTT 上行
 *
 * 状态说明（不过度包装）：这个实现只做到「接口就位 + 能编译」。
 * 本机没有 broker，所以它没有被联调过，只验证了构建。
 * 仓库的 CI 也只编译、不运行它 —— 假装它跑通了没有意义。
 *
 * 没有 Paho 开发包的机器上（比如 CI 容器），整个实现退化为「总是失败」，
 * 但要保证能编译。否则一个未联调的模块会把整条构建卡住。
 *
 * 真实设备上还缺三样，写在这里免得以后忘：
 *   1. 断线重连要有退避（1s / 2s / 4s … 上限 30s），不能死循环重试
 *   2. publish 失败要能落盘缓存，恢复后补发
 *   3. QoS 按数据类型分：告警用 QoS1，周期遥测用 QoS0
 */
#include "mg_uplink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

#ifdef MG_HAVE_MQTT

#include <MQTTClient.h>

static MQTTClient g_client;
static int g_connected;
static const char *g_addr  = "tcp://127.0.0.1:1883";
static const char *g_topic = "gw/uplink";
static const char *g_cid   = "mcu-gateway";

static int up_open(void)
{
    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    int rc;

    /* 允许用环境变量覆盖，避免把地址写死在固件里 */
    if (getenv("MG_MQTT_ADDR") != 0)
        g_addr = getenv("MG_MQTT_ADDR");
    if (getenv("MG_MQTT_TOPIC") != 0)
        g_topic = getenv("MG_MQTT_TOPIC");

    rc = MQTTClient_create(&g_client, g_addr, g_cid,
                           MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        log_error("mqtt: create failed (%d)", rc);
        return -1;
    }

    opts.keepAliveInterval = 30;
    opts.cleansession = 1;
    opts.connectTimeout = 5;

    rc = MQTTClient_connect(g_client, &opts);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        log_error("mqtt: connect %s failed (%d), broker not running?", g_addr, rc);
        MQTTClient_destroy(&g_client);
        return -1;
    }

    g_connected = 1;
    log_info("mqtt: connected to %s, topic %s", g_addr, g_topic);
    return 0;
}

static int up_publish(const char *json)
{
    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    if (json == 0 || !g_connected)
        return -1;

    msg.payload = (void *)json;
    msg.payloadlen = (int)strlen(json);
    msg.qos = 0;               /* 遥测用 QoS0：丢了下一周期还有 */
    msg.retained = 0;

    rc = MQTTClient_publishMessage(g_client, g_topic, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        log_warn("mqtt: publish failed (%d)", rc);
        return -1;
    }

    return 0;
}

static void up_close(void)
{
    if (g_connected)
    {
        MQTTClient_disconnect(g_client, 1000);
        MQTTClient_destroy(&g_client);
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

static void up_close(void)
{
}

#endif /* MG_HAVE_MQTT */

static const mg_uplink_ops_t g_mqtt_ops =
{
    up_open, up_publish, up_close
};

const mg_uplink_ops_t *mg_uplink_mqtt_ops(void)
{
    return &g_mqtt_ops;
}
