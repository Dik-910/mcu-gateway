# mcu-gateway

基于 FreeRTOS 的多协议接入网关：把总线上的设备数据采集上来，转换成统一格式上报；
把云端下发的指令翻译成设备能懂的报文。

跑在 MCU 上（目标 STM32F103），但**核心逻辑可以在 PC 上完整验证** ——
用 FreeRTOS 的 POSIX 移植，任务、队列、抢占、栈水位都是真的内核行为，不是模拟。

## PC 侧验证

平台相关部分限定在通道驱动与 RTOS port 层，其余逻辑在 PC 上运行：

```
                 PC 上验证                    目标板上
通道实现    SocketCAN / 内存回环    ──换──>    bxCAN / UART
RTOS        FreeRTOS POSIX 移植    ──同──>    FreeRTOS Cortex-M 移植
业务逻辑    任务、队列、会话、JSON  ──完全不变──>
```

除了通道驱动和 port 层，其余代码不变。

## 架构

```
  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
  │ rx_task[0]   │  │ rx_task[1]   │  │ rx_task[n]   │   每通道一个，只收不解析
  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
         │                 │                 │
         └────────────────►│◄────────────────┘
                    ┌──────▼───────┐
                    │   rx_queue   │  定长结构体，拷贝语义
                    └──────┬───────┘
                    ┌──────▼───────┐
                    │  parse_task  │  CRC 校验 → 会话匹配 → JSON
                    └──────┬───────┘
                    ┌──────▼───────┐
                    │   uplink     │  stdout / MQTT / （预留）LoRa
                    └──────────────┘

  ┌──────────────┐
  │  poll_task   │  周期下发请求，占会话位
  └──────────────┘
  ┌──────────────┐
  │ monitor_task │  统计 + 栈水位
  └──────────────┘
```

三个设计取舍：

**1. 收帧按通道拆，解析收成一份**

常见写法是「一个通道配一个完整处理任务」，每个通道各写一份解析 + 上报。
增加第三路时只能复制，三份代码随后产生分歧。本实现把「收」按通道拆（收敛延迟），
把「处理」合并成一份（保证一致）。

代价是 `parse_task` 成为单点：其中不能有阻塞操作，因此上行必须是短操作。

**2. 队列投递一律非阻塞**

```c
if (xQueueSend(gw->rx_q, &f, 0) != pdPASS)
    gw->rx_dropped++;
```

用 `portMAX_DELAY` 的话，某个通道队列堵塞会连带拖住接收任务本身 ——
等于把「处理慢」放大成「收不到」。宁可丢帧并计数，让统计暴露问题。

**3. 请求-响应建模成显式会话**

见下节。

## 会话状态机

设备侧通信的基本模式是「发一个请求，等一个响应」。常见的简化实现用一个静态对象累积响应数据，
以「字段是否已存在」判断当前是第几个响应：

```c
if (字段已存在) 填第二个字段; else 填第一个字段;
```

该写法缺少事务标识、超时与清理。一旦中间丢帧，对象无法凑齐也不会被回收，
后续请求叠加在同一个残缺对象上——网关进程仍在运行，但不会再正确上报。

本实现将请求-响应建模为显式会话：

```
begin   ──> PENDING ──响应到达──> DONE ──> release ──> FREE
                 │
                 └──超时──> TIMEOUT ──reap（上报）──> FREE
```

- 每次请求分配自增 `sid`，响应按 `(从机, 功能码)` 匹配最早发出且仍在等待的会话
- 超时由 `expire` 标记、`reap` 回收，并上报一条 timeout 事件；
  静默丢弃会导致界面上该测点长期显示上一次的值
- 槽位数量有上限，满时 `begin` 返回失败，不驱逐在途会话

### 超时会话未回收（已修复，用例保留）

早期实现只有 `expire`（标记超时），没有 `reap`（释放槽位）。
超时会话停留在 `TIMEOUT` 状态且槽位不回收，表满后 `begin` 持续失败，
网关不再发出任何请求。

端到端测试的观测值：

```
修复前  Phase B 结束 started=22   Phase C 恢复后 started=22   ← 卡死
修复后  Phase B 结束 started=29   Phase C 恢复后 started=37   ← 继续工作
```

`completed > 0` 这一断言在缺陷存在时仍然通过（历史计数不为零），
因此最终断言改为「`started` 必须继续增长」。

## 通道抽象

```c
typedef struct {
    int  (*open)(mg_channel_t *ch, const char *arg);
    int  (*recv)(mg_channel_t *ch, mg_frame_t *f);   /* 非阻塞 */
    int  (*send)(mg_channel_t *ch, const mg_frame_t *f);
    void (*close)(mg_channel_t *ch);
} mg_channel_ops_t;
```

已有实现：

| 实现 | 用途 |
|---|---|
| `mg_chan_socketcan.c` | Linux SocketCAN，vcan 上可用 |
| `mg_chan_loopback.c` | 内存回环，模拟一个双向对端，测试用 |
| （待写）`mg_chan_bxcan.c` | STM32 bxCAN |
| （待写）`mg_chan_uart.c` | 串口 + Modbus RTU 帧定界 |

回环实现刻意保留两条真实特性：队列定深、满了丢弃计数；逐帧非阻塞取出。
只做「存进去、取出来」的桩，只能证明代码能编译，证明不了调用方处理了这些情况。

**两个方向必须隔离**：网关 `send` 出去的帧不能出现在自己的 `recv` 里。
否则请求会被当成响应解析，会话匹配立刻错乱 —— 而这种错在代码上很难看出来。

## Modbus 承载在 CAN 上的限制

本项目支持把 Modbus RTU 帧放进 CAN 数据段（ID 标识设备），
但**只支持单点读写**：请求固定读 1 个寄存器 / 1 个线圈。

原因是长度：单点读的响应是 `地址 + 功能码 + 字节数 + 数据 + CRC` = 7 字节，
刚好塞进 8 字节；要读多个寄存器，响应必然超过 8 字节，这套映射立刻失效。

所以 `count` 在接口上被硬编码为 1。要读多个点就发多次请求 ——
这比「看起来支持批量、实际会截断」安全。

注意：CAN 上承载 Modbus 属于私有映射，不是标准做法。同类场景的标准化协议为
CANopen 与 J1939，需要互操作性时应改用其中之一。

## 构建与验证

```bash
make            # 编译 gw-demo（无硬件演示）与 gw-mqtt（MQTT 联调）
make test       # 运行全部单元测试
./gw-demo       # 回环通道 + 模拟从机，不需要任何外部依赖

# MQTT 联调需要 broker 在 1883 监听
make broker     # 没有 mosquitto 时用仓库自带的 amqtt 版 broker（需 pip install amqtt）
make interop    # 起网关 + 跑上下行闭环验证
```

`make test` 共 114 项断言：

| 套件 | 覆盖 |
|---|---|
| `mg_session_test` | 45 项：FIFO 匹配、超时、回收、表满、tick 回绕、sid 回绕 |
| `mg_modbus_test` | 27 项：CRC 已知值、组帧字节精确、异常响应、CRC 错误拒绝 |
| `mg_channel_test` | 29 项：双向隔离、参数校验、队列定深、dlc 越界拦截 |
| `mg_flow_test` | 13 项：**真实 FreeRTOS 上跑端到端**，含从机掉线与恢复 |

另外 `make interop` 有 20 项 MQTT 联调断言（需要 broker），见下节。

CRC 的期望值不是用被测函数生成的，而是先用 Python 独立实现算出来，
再用 Modbus 文档里的已知样例（`01 04 00 00 00 01` → `31 CA`）交叉核对。

演示输出：

```
[uplink:stdout] #1 {"gw":"mcu-gateway","ch":"modbus0","type":"modbus",
                     "slave":1,"func":3,"sid":1,"ts":107,"value":0}
stats: rx_drop=0 parse_err=0 json_err=0 up_err=0
       sess start=119 done=119 timeout=0 unmatched=0
  ch[0] modbus0 rx=119 tx=119 rx_err=0 tx_err=0
  stack rx[0] free=1019 words
  stack parse free=2043 words
```

栈水位是给目标板调栈大小的依据。不测这个，栈溢出只会表现为随机死机。

## MQTT 联调

联调最容易做假的地方是**只测一个方向**：能看到上行有数据就宣布成功。
但一个只会发不会收的客户端也能通过这种测试。

所以联调脚本验证的是闭环：

```
PASS  uplink carries modbus samples  (23 msgs)
PASS  baseline value is 200 (reg*100)
PASS  published gw/downlink: {'slave': 1, 'reg': 2, 'value': 777}
PASS  gateway acknowledged the write  (1 ack)
PASS  ack carries new value 777
PASS  gateway polled the written register again
PASS  readback reflects the write (777)  (values seen: [777])
PASS  gateway keeps polling after bad commands
Result: 20 passed, 0 failed
```

最后那条 `readback reflects the write` 是关键：
云端下发的值必须真的经过「解析 JSON → 构造 Modbus 写请求 → 从机改寄存器 →
下一轮轮询读回」这条链路，才会出现在上行里。任何一环断掉，那条断言就红。

### 三处实现细节

**1. 回调不能直接调 FreeRTOS API**

Paho 的消息回调跑在它自己的线程里（PC）或需要主动 yield（MCU），
都不是 RTOS 任务上下文。在里面调 `xQueueSend` 在 POSIX 移植上也许能跑，
到 Cortex-M 上直接崩。

所以下行数据走单生产者/单消费者环形缓冲：回调只写 `head`，`cmd_task` 只写 `tail`，
不需要锁。

**2. 重连不能放在 publish 里**

`MQTTClient_connect` 可能阻塞数秒。顺手在 publish 里重连，会把调用它的解析任务
一起卡住，采集直接停摆。所以有独立的 `comm_task` 负责 tick 重连，
退避 1s → 2s → 4s → … → 30s 封顶。

**3. 写响应要单独解析**

Modbus 写响应（`0x06`）没有「字节数」字段，直接回显请求的地址和值。
塞进读响应的解析路径会解出错的值 —— 取到的是 `reg` 和 `value` 的高字节拼起来。

### broker 的选型

CI 使用 **mosquitto**（`apt install mosquitto`），本地没有 mosquitto 时
使用 `tools/run_broker.py`（基于 amqtt，独立的 Python 实现）。

不使用自研 broker：被测客户端与测试用 broker 由同一套代码实现时，
格式错误在两侧同时发生，用例仍会通过。第三方实现才能验证协议符合性，
与 UDS 侧使用 udsoncan 的理由相同。

## 目录

```
src/util/mg_types.h        跨任务传递的定长结构体
src/channel/               通道抽象与两个实现
src/protocol/mg_modbus.*   Modbus RTU 组帧 / CRC / 解析
src/protocol/mg_session.*  请求会话表
src/app/mg_app.*           任务组装
src/app/mg_uplink*.*       上行抽象（stdout / MQTT）
thirdparty/freertos/       FreeRTOS 内核（vendor，仅保留必要文件）
examples/gw_demo.c         无硬件演示
test/                      单元测试
```

## 已知限制

诚实列出，避免误判成熟度：

- **MQTT 只在 QoS0 上验证过**：QoS1 的重传、去重、持久会话都没测。
- **断线期间的数据直接丢**：没有落盘缓存和恢复后补发。
- **STM32 平台未实现**：`mg_chan_bxcan.c` 和 Cortex-M port 都还没写，需要硬件。
- **LoRa / NB-IoT 未接入**：通道抽象留了位置，没有实现。
- **Modbus 从机模式未做**：只有主站（网关主动轮询）。
- **参数无持久化**：配置目前编译期写死，没有 Flash 参数区。
- **无看门狗**：目标板上需要独立看门狗 + 主循环喂狗。
- **无断线缓存**：上行失败的数据直接丢弃，没有落盘重发。

## 许可证

木兰宽松许可证 v2，见 [LICENSE](LICENSE)。
FreeRTOS 内核为 MIT，保留原版权声明于 `thirdparty/freertos/`。
cJSON 为 MIT。
