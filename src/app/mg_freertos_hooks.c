/*
 * mg_freertos_hooks.c —— FreeRTOS 钩子
 *
 * 三个钩子都是「最后一道防线」：
 *   vAssertCalled              内核断言失败，说明调用方式用错了
 *   vApplicationMallocFailedHook  堆耗尽，通常是任务栈开太大或队列太深
 *   vApplicationStackOverflowHook  某任务栈溢出
 *
 * MCU 上的正确做法是：记录现场 -> 喂狗超时 -> 复位。
 * 直接在钩子里复位会丢掉现场，事后什么都查不到。
 *
 * PC 上相反：直接 abort，让测试立刻红掉。
 * 在 PC 上忍着一个 assert 继续跑，等于把问题留到目标板上才暴露。
 */
#include <stdio.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"

void vAssertCalled(const char *file, int line)
{
    printf("\n[FATAL] assert failed at %s:%d\n", file ? file : "?", line);
    fflush(stdout);
    abort();
}

void vApplicationMallocFailedHook(void)
{
    printf("\n[FATAL] pvPortMalloc failed: heap exhausted "
           "(configTOTAL_HEAP_SIZE=%u)\n", (unsigned)configTOTAL_HEAP_SIZE);
    fflush(stdout);
    abort();
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("\n[FATAL] stack overflow in task '%s'\n",
           pcTaskName ? pcTaskName : "?");
    fflush(stdout);
    abort();
}
