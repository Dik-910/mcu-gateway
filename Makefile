# mcu-gateway
#
# 构建目标：
#   make          编译演示程序 gw-demo
#   make test     运行单元测试（不依赖硬件、不依赖 vcan）
#   make clean

CC      ?= gcc
CFLAGS  ?= -std=gnu99 -Wall -Wextra -O1 -g
CFLAGS  += -I. \
           -Isrc/util -Isrc/channel -Isrc/protocol -Isrc/app \
           -Ithirdparty/freertos/include -Ithirdparty/freertos/port -Ithirdparty/freertos \
           -Ithirdparty/cJSON -Ithirdparty/log

LDLIBS  += -lpthread -Wl,--no-as-needed -lgcc_s

# Paho 只在装了开发包时才编进去。
# 这样仓库在任何机器上都能构建 —— MQTT 实现没联调过，
# 不应该因为它挡住整条 CI。
HAVE_MQTT := $(shell test -f /usr/include/MQTTClient.h && echo 1)
ifeq ($(HAVE_MQTT),1)
CFLAGS  += -DMG_HAVE_MQTT
LDLIBS  += -lpaho-mqtt3c
endif

KERNEL := thirdparty/freertos/tasks.c \
          thirdparty/freertos/queue.c \
          thirdparty/freertos/list.c \
          thirdparty/freertos/port/port.c \
          thirdparty/freertos/utils/wait_for_event.c \
          thirdparty/freertos/memmang/heap_4.c

CORE   := src/channel/mg_channel.c \
          src/channel/mg_chan_loopback.c \
          src/channel/mg_chan_socketcan.c \
          src/protocol/mg_modbus.c \
          src/protocol/mg_session.c \
          src/app/mg_app.c \
          src/app/mg_uplink_stdout.c \
          src/app/mg_uplink_mqtt.c \
          src/app/mg_freertos_hooks.c \
          thirdparty/cJSON/cJSON.c \
          thirdparty/log/log.c

ALL_SRC := $(CORE) $(KERNEL)

.PHONY: all test clean

all: gw-demo

gw-demo: examples/gw_demo.c $(ALL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# ---- 单元测试 ----
#
# 纯逻辑部分（会话表、Modbus 组帧、通道抽象）不需要 FreeRTOS 也能测，
# 所以单独编，跑得快、依赖少。涉及任务的才连内核。

test/mg_session_test: test/mg_session_test.c src/protocol/mg_session.c
	$(CC) $(CFLAGS) $^ -o $@

test/mg_modbus_test: test/mg_modbus_test.c src/protocol/mg_modbus.c
	$(CC) $(CFLAGS) $^ -o $@

test/mg_channel_test: test/mg_channel_test.c src/channel/mg_channel.c src/channel/mg_chan_loopback.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

test/mg_flow_test: test/mg_flow_test.c $(ALL_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

TESTS := test/mg_session_test test/mg_modbus_test test/mg_channel_test test/mg_flow_test

test: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; done

clean:
	$(RM) gw-demo $(TESTS)
