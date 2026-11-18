# mcu-gateway
#
# 构建目标：
#   make          编译演示程序 gw-demo
#   make test     运行单元测试（不依赖硬件、不依赖 vcan）
#   make clean

CC      ?= gcc
# PATH 上的 python3 可能是某个缺标准库的托管运行时，
# 显式给一个默认值，需要时用 make PYTHON=... 覆盖
PYTHON  ?= /usr/bin/python3
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
# 注意：用 CPPFLAGS 而不是 CFLAGS。
# 命令行传 CFLAGS="..." 时（比如 CI 里的 -Werror 检查）会覆盖 Makefile 的 +=，
# 若把 -DMG_HAVE_MQTT 放在 CFLAGS 里就会悄悄丢掉，编出没有 MQTT 的二进制，
# 而 interop 测试仍然"跑得很正常"——只是它测的是 stub。
ifeq ($(HAVE_MQTT),1)
CPPFLAGS += -DMG_HAVE_MQTT
LDLIBS   += -lpaho-mqtt3c
endif

KERNEL := thirdparty/freertos/tasks.c \
          thirdparty/freertos/queue.c \
          thirdparty/freertos/list.c \
          thirdparty/freertos/port/port.c \
          thirdparty/freertos/utils/wait_for_event.c \
          thirdparty/freertos/memmang/heap_4.c

CORE   := src/util/mg_crc16.c \
          src/channel/mg_channel.c \
          src/channel/mg_chan_loopback.c \
          src/channel/mg_chan_socketcan.c \
          src/protocol/mg_modbus.c \
          src/protocol/mg_session.c \
          src/app/mg_app.c \
          src/app/mg_outbox.c \
          src/app/mg_outbox_file.c \
          src/app/mg_param.c \
          src/app/mg_param_file.c \
          src/app/mg_uplink_stdout.c \
          src/app/mg_uplink_mqtt.c \
          src/app/mg_freertos_hooks.c \
          thirdparty/cJSON/cJSON.c \
          thirdparty/log/log.c

ALL_SRC := $(CORE) $(KERNEL)

.PHONY: all test clean

all: gw-demo gw-mqtt

gw-demo: examples/gw_demo.c $(ALL_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

gw-mqtt: examples/gw_mqtt.c $(ALL_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

# ---- 单元测试 ----
#
# 纯逻辑部分（会话表、Modbus 组帧、通道抽象）不需要 FreeRTOS 也能测，
# 所以单独编，跑得快、依赖少。涉及任务的才连内核。

test/mg_session_test: test/mg_session_test.c src/protocol/mg_session.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

test/mg_modbus_test: test/mg_modbus_test.c src/protocol/mg_modbus.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

test/mg_channel_test: test/mg_channel_test.c src/channel/mg_channel.c src/channel/mg_chan_loopback.c thirdparty/log/log.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

test/mg_flow_test: test/mg_flow_test.c $(ALL_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

# outbox 是纯逻辑 + 文件后端，不依赖 FreeRTOS，单独编
test/mg_outbox_test: test/mg_outbox_test.c src/app/mg_outbox.c src/app/mg_outbox_file.c \
                     src/util/mg_crc16.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

# 参数区：纯逻辑 + 文件后端
test/mg_param_test: test/mg_param_test.c src/app/mg_param.c src/app/mg_param_file.c \
                    src/app/mg_outbox_file.c src/util/mg_crc16.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

TESTS := test/mg_session_test test/mg_modbus_test test/mg_channel_test test/mg_flow_test \
         test/mg_outbox_test test/mg_param_test

# MQTT 联调：需要 broker 已在 1883 监听（make broker 可以起一个）。
# Paho 未安装时明确跳过，而不是假装通过。
.PHONY: interop outbox-check broker
interop: gw-mqtt
	@if [ -z "$(HAVE_MQTT)" ]; then \
	  echo "SKIP: Paho C not found (install libpaho-mqtt-dev)"; exit 0; fi
	@echo "--- starting gw-mqtt (18s) ---"
	@MG_OUTBOX_FILE=.gw-mqtt.outbox ./gw-mqtt --seconds 18 > .gw-mqtt.log 2>&1 & \
	 GW=$$!; sleep 2; \
	 $(PYTHON) tools/mqtt_interop.py; RC=$$?; \
	 kill $$GW 2>/dev/null; wait $$GW 2>/dev/null; \
	 echo "--- gw-mqtt log ---"; tail -8 .gw-mqtt.log; \
	 exit $$RC

# QoS1 与断线补发：自己起停 mosquitto，不需要外部 broker
outbox-check: gw-mqtt
	@if [ -z "$(HAVE_MQTT)" ]; then \
	  echo "SKIP: Paho C not found"; exit 0; fi
	@$(PYTHON) tools/outbox_interop.py

# 本地没有 mosquitto 时用独立实现的 broker（需要 pip install amqtt）
broker:
	@$(PYTHON) tools/run_broker.py

test: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; done

clean:
	$(RM) gw-demo gw-mqtt $(TESTS)
