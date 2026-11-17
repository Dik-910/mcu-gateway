# STM32 移植清单

目标：把 PC 上验证过的逻辑移植到 STM32F103（或同系列）。
本文只列出必须修改的部分与已知易错点，不复述参考手册。

## 一、要换的三层

| 层 | PC 上 | 目标板 | 工作量 |
|---|---|---|---|
| RTOS 移植 | `thirdparty/freertos/port/port.c`（POSIX） | Cortex-M3 port（`portable/GCC/ARM_CM3`） | 换文件，改 `FreeRTOSConfig.h` |
| 通道实现 | `mg_chan_socketcan.c` | 新写 `mg_chan_bxcan.c` | 约 200 行 |
| 时间基准 | `xTaskGetTickCount()` | 同（`SysTick` 驱动） | 无需改 |

**业务层（`mg_app.c` / `mg_session.c` / `mg_modbus.c` / `mg_uplink*`）一行都不用改。**
这一条如果做不到，说明抽象层没设计对。

## 二、bxCAN 的接口映射

`mg_channel_ops_t` 的四个函数对应关系：

| ops | HAL 调用 | 要点 |
|---|---|---|
| `open` | `HAL_CAN_ConfigFilter()` + `HAL_CAN_Start()` + `HAL_CAN_ActivateNotification()` | 三步都要，少一步收不到中断 |
| `recv` | 从队列取（中断里投递） | 不要在任务里轮询 FIFO |
| `send` | `HAL_CAN_AddTxMessage()` | 先查 `HAL_CAN_GetTxMailboxesFreeLevel()` |
| `close` | `HAL_CAN_Stop()` + `HAL_CAN_DeactivateNotification()` | 通常用不上 |

### 接收要用中断，不要轮询

PC 上 `rx_task` 里有个 `vTaskDelay(1)` 让出 CPU。到 MCU 上要换成中断唤醒：

```c
/* 中断回调里只投递，不处理 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    mg_frame_t f;
    BaseType_t woken = pdFALSE;

    /* 一次中断里把 FIFO 读空，不然会漏帧 */
    while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0)
    {
        if (read_one_frame(hcan, &f) == 0)
            xQueueSendFromISR(g_rx_q, &f, &woken);
    }

    portYIELD_FROM_ISR(woken);
}
```

中断内必须读空 FIFO：深度只有 3 级，处理期间若有 3 帧到达，第 4 帧由硬件丢弃且不报错。

## 三、三处易错点

### 1. 32 位掩码模式下 FR2 的语义

```
FilterIdHigh/Low   = FR1  →  期望的 ID
FilterMaskIdHigh/Low = FR2 →  掩码
```

**FR2 某位为 1 表示「这一位必须和 FR1 一致」，为 0 表示「不关心」。**

该语义与常见直觉相反（容易理解为 mask=1 表示忽略）。配置错误时，
过滤器要么放行所有帧，要么一帧都不放行。

全接收（调试用）：

```c
sFilterConfig.FilterIdHigh = 0x0000;
sFilterConfig.FilterIdLow  = 0x0000;
sFilterConfig.FilterMaskIdHigh = 0x0000;
sFilterConfig.FilterMaskIdLow  = 0x0000;   /* 全 0 = 全部接收 */
```

只收 `0x180`–`0x18F`（高 7 位固定）：

```c
/* 期望 ID = 0x180，掩码 = 0x7F0（左移 5 位放进寄存器的高位区） */
sFilterConfig.FilterIdHigh = 0x180 << 5;
sFilterConfig.FilterMaskIdHigh = 0x7F0 << 5;
```

注意 11 位标准帧在 32 位寄存器中左对齐，需要左移 5 位。该对齐方式在参考手册中不易察觉，
遗漏则收不到任何帧。

### 2. STM32 的位时序定义和标准 CAN 不同

标准 CAN 把位时间分成 **同步段 + 传播段 + 相位缓冲段1 + 相位缓冲段2**；
STM32 的 bxCAN **把传播段和相位缓冲段1 合并成一个 `BS1`**。

所以按标准公式算出来的 `BS1` 直接填进去会得到错误的采样点。
配 500 kbps / 8 MHz APB1 的常用值：`Prescaler=1, BS1=13, BS2=2, SJW=1`
（1 + 13 + 2 = 16 tq，采样点在 87.5%）。

配完之后**必须上示波器或读回 `CAN_BTR` 确认**，不能只看代码。

### 3. 发送邮箱只有 3 个

`HAL_CAN_AddTxMessage` 在邮箱满时返回 `HAL_ERROR`。
不应写成 `while (free == 0 && timeout--)` 的忙等循环：在 RTOS 任务中忙等会阻塞该任务。

正确做法是返回失败并计数，由上层决定是否重发：

```c
if (HAL_CAN_GetTxMailboxesFreeLevel(hcan) == 0)
    return -1;      /* 让调用方计数 */
```

## 四、先用环回静默模式自检

硬件首次上电时不接总线，使用**环回静默模式**（Loopback + Silent）：

```c
hcan.Init.Mode = CAN_MODE_SILENT_LOOPBACK;
```

- 发出的帧能被自身接收，说明寄存器配置、过滤器、中断链路均正确
- 不驱动总线，总线上存在其他节点时不会产生干扰

该步骤通过后再切换到 `CAN_MODE_NORMAL` 接入真实总线。

另外两种模式的差别：`CAN_MODE_LOOPBACK` 会实际向总线发送，其他节点可收到；
`CAN_MODE_SILENT` 只接收不发送，适合抓包分析。

## 五、RAM 预算（F103C8：20 KB）

PC 上的配置不能直接搬：

| 项 | PC 值 | 建议 MCU 值 | 说明 |
|---|---|---|---|
| `configTOTAL_HEAP_SIZE` | 256 KB | **8 KB** | 所有任务栈 + 队列都从这里分配 |
| `configMINIMAL_STACK_SIZE` | 1024 | **128** | 单位是字（4 字节），128 = 512 字节 |
| rx 任务栈 | 1024 | **256** | 只做一次 memcpy |
| parse 任务栈 | 2048 | **512** | cJSON 递归解析吃栈 |
| rx_queue 深度 | 32 | **8–16** | 每项 24 字节，32 项 = 768 字节 |

算一下：8 KB heap 里要放 4 个任务栈（约 2 KB）+ 队列（384 B）+ 空闲任务和定时器任务
（约 1 KB），剩 4 KB 余量 —— 够，但不宽裕。

**调栈大小的依据是 PC 上跑出来的水位。** 本项目实测：

```
stack rx[0] free=1019 words   → 用了 5 words ≈ 20 字节（PC 上是 8 字节/字）
stack parse free=2043 words
```

PC 上的字长和栈开销都和目标板不同，**只能当参考，不能直接换算**。
正确做法是 MCU 上再跑一遍 `uxTaskGetStackHighWaterMark`，留 30% 余量。

## 六、移植顺序（每步都能验证）

1. **点灯 + 串口打印** —— 确认工具链和时钟
2. **`CAN_MODE_SILENT_LOOPBACK` 发一帧收一帧** —— 确认 CAN 外设配置
3. **接上 `mg_chan_bxcan.c` + 回环从机** —— 确认通道抽象能对上
4. **切 `CAN_MODE_NORMAL`，用另一块板或 CAN 分析仪对发** —— 确认真实总线
5. **接 Modbus（串口）** —— 需要先解决帧定界：RTU 用 3.5 字符时间的静默间隔

第 2 步是分水岭。这一步过了，剩下的都是体力活；过不了，先查过滤器和位时序。

## 七、还没做的部分

- `mg_chan_bxcan.c` —— 未实现
- `mg_chan_uart.c`（Modbus RTU 帧定界 + 定时器）—— 未实现
- 独立看门狗（IWDG）—— 未实现
- Flash 参数区 —— 未实现
- 掉电检测与安全关机 —— 未实现

这些都需要硬件才能验证，写出来也只是猜。等板子到了再补。
