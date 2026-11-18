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
make outbox-check  # QoS1 与断线补发，自己起停 mosquitto，不需要外部 broker
```

`make test` 共 231 项断言：

| 套件 | 覆盖 |
|---|---|
| `mg_session_test` | 45 项：FIFO 匹配、超时、回收、表满、tick 回绕、sid 回绕 |
| `mg_modbus_test` | 27 项：CRC 已知值、组帧字节精确、异常响应、CRC 错误拒绝 |
| `mg_channel_test` | 29 项：双向隔离、参数校验、队列定深、dlc 越界拦截 |
| `mg_flow_test` | 13 项：**真实 FreeRTOS 上跑端到端**，含从机掉线与恢复 |
| `mg_outbox_test` | 59 项：顺序与确认、满环丢最旧、三处掉电点、坏块、重启恢复、文件后端 |
| `mg_param_test` | 58 项：编解码、字段校验、双页交替、新页损坏回落、保存掉电、文件后端 |

另外 `make interop` 有 20 项 MQTT 联调断言、`make outbox-check` 有 11 项投递语义断言，
两者都需要 broker（前者需外部 broker，后者自己起停）。

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

## 投递语义与断线补发

上行使用 QoS1，并且**先落盘再发**。broker 掉线、网关自己重启，未确认的数据
都还在队列里，重连后按原顺序补发。判定「已投递」的唯一依据是收到 PUBACK。

### 存储布局

```
头部 16B：magic(4) 版本(1) 保留(3) read_seq(4) CRC16(2) 保留(2)
槽  216B：状态(1) 长度(1) 保留(2) 序号(4) CRC16(2) 保留(6) 载荷(200)
```

序号取模定位槽位，环形复用。128 个槽共约 27KB，按本机 10 条/秒的采集速率
覆盖约 13 秒断线；采样更慢的现场覆盖时间更长。

### 三个设计点

**1. 提交点是状态字节，不是「写完数据」**

写一条记录的顺序固定为：失效(`0x00`) → 长度/序号/CRC/载荷 → 有效(`0xA5`)。
在状态字节落下之前掉电，读回来是空槽；而 CRC 负责挡住另一种情况——
状态字节还是上一轮残留的 `0xA5`，数据却只写了一半。

**2. 头部只存读取位置**

`write_seq` 不落盘，由槽里的最大序号反推。少一个持久化字段，
就少一种「两个指针互相矛盾」的失效模式。头部本身丢了（擦写时掉电）时
从槽位反推读取位置，代价是已确认的那条会被重发一次——宁可重复，不可丢失。

**3. 满环丢最旧**

反过来（拒收新的）会让故障期间整段数据全缺：旧数据即使发出去也已过时，
新数据反映的才是当前状态。丢弃条数计入 `dropped`，在重连日志里可见。

### 语义边界

- **at-least-once**：PUBACK 丢失时会重发同一条。消费端需要按业务键去重，
  「恰好一次」在协议层做不到。
- **队列不是数据库**：写入不做 `fsync`，掉电时最后几条可能丢。
  这是为 Flash 寿命和吞吐做的取舍。
- **单写者**：outbox 由 `comm_task` 独占，回调和解析任务只调 `publish`
  入队，不直接碰存储。

### 验证

```bash
make outbox-check     # 自己起停 mosquitto，不需要外部 broker
```

三个阶段分别对应现场会遇到的三种情况：

| 阶段 | 做法 | 断言 |
|---|---|---|
| A 正常链路 | broker 在线 | 消息按序到达、载荷字段完整 |
| B broker 掉线 | 杀掉 broker，网关继续采集 4 秒后恢复 | 补发消息的 `ts` 紧接断线前最后一条（实测间隔 100ms，即一个轮询周期），断线窗口内无大于 500ms 的空洞 |
| C 网关重启 | broker 与网关同时停，再依次恢复 | 新进程（`ts` 从 0 数起）能收到重启前采到的数据，说明记录来自磁盘而非重新采集 |

阶段 B 的判据值得说明：如果补发不生效，重连后收到的会是**新采**的数据，
其 `ts` 比断线前大 4 秒以上；实测间隔只有 100ms，说明队列里存的确实是积压。

## 参数区

通道表、轮询点、超时、MQTT 地址这些「现场要改的东西」不再写在源码里，
改一个从机地址不必重新出固件。

### 双页交替

```
[页 0: 页头 16B + 负载 ≤496B] [页 1: 同上]
```

两页各带序号，写入永远写「非当前有效」的那一页：

```
写之前：[页0 seq=7 有效][页1 seq=6 旧]
写页1（掉电在这里）→ 页0 仍然有效，页1 是垃圾
写完校验通过 → 页1 成为有效页，seq=8
```

单页原地覆盖的问题在于「写坏」比「不写」更糟：参数和 CRC 一起没了，
设备只能回默认值，而默认值未必适合现场。代价是可用容量减半。

提交点是页头的 magic：先写负载，再写页头（magic 位保持 0），最后写 magic。
第二步之后掉电，页头是「旧 magic + 新 CRC」，必然校验不过，于是回落到另一页。

### 显式编码，不存结构体

`memcpy(struct)` 存下来的东西换编译器/换平台读回来就是乱码，而且错得很安静：
CRC 只会说「对不上」，不会告诉你是哪个字段对不上。所以逐字节编码，
长度前缀的字符串，解码时每读一个字段都检查剩余长度。

解码还会拒绝两种情况：版本号不认识，以及尾部有多余的非零字节——
后者说明写入方比读取方新，拿半份配置去跑比不跑更危险。

### 校验在使用前

`mg_param_validate` 单独成一个函数，不从 load 里顺带做：参数将来可能从
串口或云端灌进来，那条路径也必须过同一关。检查的是硬边界——
从机地址 1..247、功能码 1..4、寄存器范围不越过 16 位空间、通道下标存在。

### 首次上电

两页都没有有效数据时用默认值，并立刻写回存储。这样「首次上电」和
「参数丢失」是同一条路径，都能自愈，不需要人工干预。

### 用法

```bash
./gw-demo                      # 首次运行生成 gw_param.bin（1KB），之后从里面读
MG_PARAM_FILE=/tmp/p.bin ./gw-mqtt   # 换一份参数分区
```

启动日志会写明参数来源：

```
param: 1 channel(s), 8 poll entries, interval 100 ms, source=page1
```

`source=defaults` 表示两页都不可用、用的是默认值并已写回；
`page0` / `page1` 是实际生效的那一页。

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
src/app/mg_outbox.c        掉电不丢的发送队列（纯逻辑，io 后端注入）
src/app/mg_outbox_file.c   PC 侧文件后端，模拟 Flash 扇区
src/app/mg_param.c         参数区：显式编码 + 双页交替
src/app/mg_param_file.c    参数区的文件后端
src/util/mg_crc16.c        存储校验用的 CRC16（唯一实现）
```

## 已知限制

诚实列出，避免误判成熟度：

- **断线超过队列容量仍会丢**：约 27KB 的 outbox 覆盖 13 秒（10 条/秒），
  更长的断线丢最旧并计入 `dropped`。目标板上换成 Flash 扇区后可以加容量，
  但仍受扇区大小限制。
- **重复消息需要消费端去重**：QoS1 是 at-least-once，PUBACK 丢失时会重发。
- **Paho 自身的持久化没开**：进程内 in-flight 由 outbox 兜底，
  但 PUBACK 窗口内崩溃仍会重发（同样是 at-least-once 的代价）。
- **STM32 平台未实现**：`mg_chan_bxcan.c` 和 Cortex-M port 都还没写，需要硬件。
- **LoRa / NB-IoT 未接入**：通道抽象留了位置，没有实现。
- **Modbus 从机模式未做**：只有主站（网关主动轮询）。
- **无看门狗**：目标板上需要独立看门狗 + 主循环喂狗。
- **参数没有在线修改入口**：只能通过 `mg_param_save` 落盘，
  还没有串口/云端下发参数的通道。校验函数已经就位，缺的是入口。

## 许可证

木兰宽松许可证 v2，见 [LICENSE](LICENSE)。
FreeRTOS 内核为 MIT，保留原版权声明于 `thirdparty/freertos/`。
cJSON 为 MIT。
