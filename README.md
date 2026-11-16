# mcu-gateway

基于 FreeRTOS 的多协议接入网关：把总线上的设备数据采集上来，转换成统一格式上报；
把云端下发��指令翻译成设备能懂的报文。

跑在 MCU 上（目标 STM32F103），但**核心逻辑可以在 PC 上完整验证** ——
用 FreeRTOS 的 POSIX 移植，任务、队列、抢占、栈水位都是真的内核行为，不是模拟。

## 为什么要在 PC 上跑

嵌入式项目最常见的困境是：板子没到手，代码一行都跑不了；等板子到了，
一半时间花在调环境上，逻辑里的问题全被硬件问题盖住。

这里的做法是把「平台相关」压缩到最小：

```
                 PC 上验证                    目标板上
通道实现    SocketCAN / 内存回环    ──换──>    bxCAN / UART
RTOS        FreeRTOS POSIX 移植    ──同──>    FreeRTOS Cortex-M 移植
业务逻辑    任务、队列、会话、JSON  ──完全不变──>
```

除了通道驱动和 port 层，其余代码一行不改。

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

课程项目常见的写法是「一个通道配一个完整处理任务」，每个通道各写一份解析 + 上报。
加第三路时复制粘贴，三份代码迟早不一致。这里把「收」按通道拆（收敛延迟），
把「处理」合并成一份（保证一致）。

代价是 `parse_task` 成了单点：它里面不能有阻塞操作。所以上行必须是短操作。

**2. 队列投递一律非阻塞**

```c
if (xQueueSend(gw->rx_q, &f, 0) != pdPASS)
    gw->rx_dropped++;
```

用 `portMAX_DELAY` 的话，某个通道队列堵塞会连带拖住接收任务本身 ——
等于把「处理慢」放大成「收不到」。宁可丢帧并计数，让统计暴露问题。

**3. 请求-响应建模成显式会话**

见下节。

## 会话状态机：这个项目最值得看的部分

设备侧通信的本质是「发一个请求，等一个响应」。很多实现靠一个静态对象攒数据，
用「字段是否已存在」判断当前是第几次响应：

```c
if (字段已存在) 填第二个字段; else 填第一个字段;
```

这种写法有三个隐患：没有事务标识、没有超时、没有清理。
**只要中间丢一帧，对象就永远凑不齐且不会回收** ——
之后所有请求都往同一个残缺对象上叠加，网关看起来还在运行，
实际再也不会正确上报一次。

这里的做法：

```
begin   ──> PENDING ──响应到达──> DONE ──> release ──> FREE
                 │
                 └──超时──> TIMEOUT ──reap（上报）──> FREE
```

- 每次请求分配自增 `sid`，响应按 `(从机, 功能码)` 匹配**最早发出且仍在等待**的会话
- 超时由 `expire` 标记、`reap` 回收，并且**上报一条 timeout 事件**，
  而不是静默丢弃 —— 否则界面上这个点会一直显示上次的值，看起来一切正常
- 槽位数量有上限，满了 `begin` 明确返回失败，不驱逐在途会话

### 一个真实的 bug（已修复，保留在测试里）

第一版只做了 `expire`（标记超时）没有做 `reap`（释放槽位）。
结果是：超时的会话永远停在 `TIMEOUT` 状态，槽位不回收，
表被占满之后 `begin` 一直失败 —— **网关活着，但再也发不出任何请求**。

端到端测试就是这么抓出来的：

```
修复前  Phase B 结束 started=22   Phase C 恢复后 started=22   ← 卡死
修复后  Phase B 结束 started=29   Phase C 恢复后 started=37   ← 继续工作
```

注意 `completed > 0` 这个断言在 bug 存在时**依然通过**（历史值不为零），
所以最终断言改成「`started` 必须继续增长」—— 断言写松了等于没写。

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

注意：**这不是标准做法**。CAN 上跑 Modbus 属于私有映射，
标准做法是 CANopen 或 J1939。这里保留它是因为课程/教学场景常见，
真做产品应该换协议。

## 构建与验证

```bash
make            # 编译演示程序 gw-demo
make test       # 运行全部单元测试
./gw-demo       # 无硬件演示：回环通道 + 模拟从机，跑 8 秒
```

`make test` 共 114 项断言：

| 套件 | 覆盖 |
|---|---|
| `mg_session_test` | 45 项：FIFO 匹配、超时、回收、表满、tick 回绕、sid 回绕 |
| `mg_modbus_test` | 27 项：CRC 已知值、组帧字节精确、异常响应、CRC 错误拒绝 |
| `mg_channel_test` | 29 项：双向隔离、参数校验、队列定深、dlc 越界拦截 |
| `mg_flow_test` | 13 项：**真实 FreeRTOS 上跑端到端**，含从机掉线与恢复 |

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

- **MQTT 未联调**：实现已就位，能编译，但本机没有 broker，从未真正收发过。
  CI 只验证构建。
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
