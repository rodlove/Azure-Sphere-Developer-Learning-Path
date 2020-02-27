/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#include "FreeRTOS.h"
#include "mt3620-baremetal.h"
#include "mt3620-gpio.h"
#include "mt3620-intercore.h" // Support for inter Core Communications
#include "printf.h"
#include "semphr.h"
#include "task.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>


// Select Azure Sphere Dev Kit
//#define SEEED_DK 1
#define SEEED_MINI_DK 1
//#define AVNET_DK = 1



/* block addresses for real time core gpio - opened in sets of four

GPIO Block first pin = 0, base address = 0x38010000
GPIO Block first pin = 4, base address = 0x38020000
GPIO Block first pin = 8, base address = 0x38030000
GPIO Block first pin = 12, base address = 0x38040000

*/

#if defined SEEED_DK || defined AVNET_DK

#define BUILTIN_LED 10
#define BUILTIN_LED_GPIO_BLOCK_FIRST_PIN 8
#define BUILTIN_LED_GPIO_BLOCK_BASE_ADDRESS 0x38030000

#elif defined SEEED_MINI_DK

#define BUILTIN_LED 7
#define BUILTIN_LED_GPIO_BLOCK_FIRST_PIN 4
#define BUILTIN_LED_GPIO_BLOCK_BASE_ADDRESS 0x38020000

#endif

#define BUTTON1_GPIO 12
#define BUTTON1_GPIO_BLOCK_FIRST_PIN 12
#define BUTTON1_GPIO_BLOCK_BASE_ADDRESS 0x38040000

#define APP_STACK_SIZE_BYTES		(512 / 4)


/// <summary>Base address of IO CM4 MCU Core clock.</summary>
static const uintptr_t IO_CM4_RGU = 0x2101000C;
static const uintptr_t IO_CM4_DEBUGUART = 0x21040000;
static const uintptr_t IO_CM4_ISU0 = 0x38070500;
static SemaphoreHandle_t LEDSemphr;
static QueueHandle_t UARTDataQueue;
static bool led1RedOn = false;

static const int blinkIntervalsMs[] = { 75, 125, 250, 500, 1000, 2000 };
static int blinkIntervalIndex = 0;
static const int numBlinkIntervals = sizeof(blinkIntervalsMs) / sizeof(blinkIntervalsMs[0]);
static const int buttonPressCheckPeriodMs = 10;

// Support for inter core communications
static BufferHeader* outbound, * inbound;
static uint32_t sharedBufSize = 0;
static const size_t payloadStart = 20;
static uint8_t buf[256];
static uint32_t dataSize;
static bool buttonPressed = false;

#ifdef SEEED_MINI_DK
static int generatePressEvent = 0;
#endif // SEEED_MINI_DK


static void ISU0_ISR(void);
static _Noreturn void DefaultExceptionHandler(void);
static _Noreturn void RTCoreMain(void);

extern uint32_t StackTop; // &StackTop == end of TCM0
extern void SVC_Handler(void);
extern void PendSV_Handler(void);
extern void SysTick_Handler(void);

// ARM DDI0403E.d SB1.5.2-3
// From SB1.5.3, "The Vector table must be naturally aligned to a power of two whose alignment
// value is greater than or equal to (Number of Exceptions supported x 4), with a minimum alignment
// of 128 bytes.". The array is aligned in linker.ld, using the dedicated section ".vector_table".

// The exception vector table contains a stack pointer, 15 exception handlers, and an entry for
// each interrupt.
#define INTERRUPT_COUNT 100 // from datasheet
#define EXCEPTION_COUNT (16 + INTERRUPT_COUNT)
#define INT_TO_EXC(i_) (16 + (i_))
const uintptr_t ExceptionVectorTable[EXCEPTION_COUNT] __attribute__((section(".vector_table")))
__attribute__((used)) = {
	[0] = (uintptr_t)&StackTop,					// Main Stack Pointer (MSP)
	[1] = (uintptr_t)RTCoreMain,				// Reset
	[2] = (uintptr_t)DefaultExceptionHandler,	// NMI
	[3] = (uintptr_t)DefaultExceptionHandler,	// HardFault
	[4] = (uintptr_t)DefaultExceptionHandler,	// MPU Fault
	[5] = (uintptr_t)DefaultExceptionHandler,	// Bus Fault
	[6] = (uintptr_t)DefaultExceptionHandler,	// Usage Fault
	[11] = (uintptr_t)SVC_Handler,				// SVCall
	[12] = (uintptr_t)DefaultExceptionHandler,	// Debug monitor
	[14] = (uintptr_t)PendSV_Handler,			// PendSV
	[15] = (uintptr_t)SysTick_Handler,			// SysTick
	[INT_TO_EXC(0)... INT_TO_EXC(46)] = (uintptr_t)DefaultExceptionHandler,
	[INT_TO_EXC(47)] = (uintptr_t)ISU0_ISR,
	[INT_TO_EXC(48)... INT_TO_EXC(INTERRUPT_COUNT - 1)] = (uintptr_t)DefaultExceptionHandler
};

static _Noreturn void DefaultExceptionHandler(void)
{
	for (;;) {
		// empty.
	}
}

static void ISU0_ISR(void)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	uint32_t iirId = ReadReg32(IO_CM4_ISU0, 0x08) & 0x1F;

	if (iirId == 0x04) {

		char data = ReadReg32(IO_CM4_ISU0, 0x00);
		xQueueSendFromISR(UARTDataQueue, &data, &xHigherPriorityTaskWoken);
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}

static void DebugUARTInit(void)
{
	// Configure UART to use 115200-8-N-1.
	WriteReg32(IO_CM4_DEBUGUART, 0x0C, 0x80); // LCR (enable DLL, DLM)
	WriteReg32(IO_CM4_DEBUGUART, 0x24, 0x3);  // HIGHSPEED
	WriteReg32(IO_CM4_DEBUGUART, 0x04, 0);    // Divisor Latch (MS)
	WriteReg32(IO_CM4_DEBUGUART, 0x00, 1);    // Divisor Latch (LS)
	WriteReg32(IO_CM4_DEBUGUART, 0x28, 224);  // SAMPLE_COUNT
	WriteReg32(IO_CM4_DEBUGUART, 0x2C, 110);  // SAMPLE_POINT
	WriteReg32(IO_CM4_DEBUGUART, 0x58, 0);    // FRACDIV_M
	WriteReg32(IO_CM4_DEBUGUART, 0x54, 223);  // FRACDIV_L
	WriteReg32(IO_CM4_DEBUGUART, 0x0C, 0x03); // LCR (8-bit word length)
}

static void ISU0Init(void)
{
	// Configure UART to use 115200-8-N-1.
	WriteReg32(IO_CM4_ISU0, 0x0C, 0x80); // LCR (enable DLL, DLM)
	WriteReg32(IO_CM4_ISU0, 0x24, 0x3);  // HIGHSPEED
	WriteReg32(IO_CM4_ISU0, 0x04, 0);    // Divisor Latch (MS)
	WriteReg32(IO_CM4_ISU0, 0x00, 1);    // Divisor Latch (LS)
	WriteReg32(IO_CM4_ISU0, 0x28, 224);  // SAMPLE_COUNT
	WriteReg32(IO_CM4_ISU0, 0x2C, 110);  // SAMPLE_POINT
	WriteReg32(IO_CM4_ISU0, 0x58, 0);    // FRACDIV_M
	WriteReg32(IO_CM4_ISU0, 0x54, 223);  // FRACDIV_L
	WriteReg32(IO_CM4_ISU0, 0x0C, 0x03); // LCR (8-bit word length)

	SetReg32(IO_CM4_ISU0, 0x04, 0x01);

	SetNvicPriority(47, 3);
	EnableNvicInterrupt(47);
}


static void PeriodicTask(void* pParameters)
{
	while (1) {
		vTaskDelay(pdMS_TO_TICKS(blinkIntervalsMs[blinkIntervalIndex]));
		xSemaphoreGive(LEDSemphr);
	}
}

static void LedTask(void* pParameters)
{
	BaseType_t rt;

	while (1) {
		rt = xSemaphoreTake(LEDSemphr, portMAX_DELAY);
		if (rt == pdPASS) {
			led1RedOn = !led1RedOn;
			Mt3620_Gpio_Write(BUILTIN_LED, led1RedOn);

#ifdef SEEED_MINI_DK
			// simulate a button press - useful for the Seeed Studio Azure Sphere Mini which does not have builtin buttons
			if (generatePressEvent++ > 50) {
				blinkIntervalIndex = (blinkIntervalIndex + 1) % numBlinkIntervals;
				buttonPressed = true;
				generatePressEvent = 0;
			}
#endif // SEEED_AZURE_SPHERE_MINI

		}
	}
}

static void ButtonTask(void* pParameters)
{
	static bool prevState = true;
	bool newState;

	while (1) {

		vTaskDelay(pdMS_TO_TICKS(buttonPressCheckPeriodMs));
		Mt3620_Gpio_Read(BUTTON1_GPIO, &newState);

		if (newState != prevState) {
			bool pressed = !newState;
			if (pressed) {
				blinkIntervalIndex = (blinkIntervalIndex + 1) % numBlinkIntervals;
				buttonPressed = true;
			}

			prevState = newState;
		}
	}
}

static void UARTTask(void* pParameters)
{
	char c;

	while (1) {
		xQueueReceive(UARTDataQueue, &c, portMAX_DELAY);
		printf("Received %c from UART\r\n", c);
	}
}

static void RTCoreMsgTask(void* pParameters)
{
	bool HLAppReady = false;

	while (1) {
		dataSize = sizeof(buf);
		int r = DequeueData(outbound, inbound, sharedBufSize, buf, &dataSize);

		//if (!(r == -1 || dataSize < payloadStart)) {
		if (r == 0 && dataSize > payloadStart){
			HLAppReady = true;
		}

		if (buttonPressed && HLAppReady) {
			const char msg[] = "ButtonPressed";
			strncpy((char*)buf + payloadStart, msg, sizeof buf - payloadStart);
			dataSize = payloadStart + sizeof msg - 1;

			EnqueueData(inbound, outbound, sharedBufSize, buf, dataSize);
			buttonPressed = false;
		}
	}
}

static void TaskInit(void* pParameters)
{
	LEDSemphr = xSemaphoreCreateBinary();

	xTaskCreate(PeriodicTask, "Periodic Task", APP_STACK_SIZE_BYTES, NULL, 6, NULL);
	xTaskCreate(LedTask, "LED Task", APP_STACK_SIZE_BYTES, NULL, 5, NULL);
	xTaskCreate(ButtonTask, "Button Task", APP_STACK_SIZE_BYTES, NULL, 4, NULL);
	xTaskCreate(UARTTask, "UART Task", APP_STACK_SIZE_BYTES, NULL, 3, NULL);
	xTaskCreate(RTCoreMsgTask, "RTCore Msg Task", APP_STACK_SIZE_BYTES, NULL, 2, NULL);

	vTaskSuspend(NULL);
}

static _Noreturn void RTCoreMain(void)
{
	// SCB->VTOR = ExceptionVectorTable
	WriteReg32(SCB_BASE, 0x08, (uint32_t)ExceptionVectorTable);

	if (GetIntercoreBuffers(&outbound, &inbound, &sharedBufSize) == -1) {
		for (;;) {
			// empty.
		}
	}

	DebugUARTInit();
	printf("FreeRTOS demo\r\n");

	ISU0Init();
	UARTDataQueue = xQueueCreate(10, sizeof(uint8_t));

	// Boost M4 core to 197.6MHz (@26MHz), refer to chapter 3.3 in MT3620 Datasheet
	uint32_t val = ReadReg32(IO_CM4_RGU, 0);
	val &= 0xFFFF00FF;
	val |= 0x00000200;
	WriteReg32(IO_CM4_RGU, 0, val);

	// LED GPIO config
	static const GpioBlock pwm2 = { .baseAddr = BUILTIN_LED_GPIO_BLOCK_BASE_ADDRESS,.type = GpioBlock_PWM,.firstPin = BUILTIN_LED_GPIO_BLOCK_FIRST_PIN,.pinCount = 4 };
	Mt3620_Gpio_AddBlock(&pwm2);
	Mt3620_Gpio_ConfigurePinForOutput(BUILTIN_LED);

	// Button GPIO config
	static const GpioBlock grp3 = { .baseAddr = BUTTON1_GPIO_BLOCK_BASE_ADDRESS,.type = GpioBlock_GRP,.firstPin = BUTTON1_GPIO_BLOCK_FIRST_PIN,.pinCount = 4 };
	Mt3620_Gpio_AddBlock(&grp3);
	Mt3620_Gpio_ConfigurePinForInput(BUTTON1_GPIO);

	xTaskCreate(TaskInit, "Init Task", APP_STACK_SIZE_BYTES, NULL, 7, NULL);
	vTaskStartScheduler();

	while (1);
}

// application hooks

void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName)
{
	;
}

void vApplicationMallocFailedHook(void)
{
	;
}

void _putchar(char character)
{
	while (!(ReadReg32(IO_CM4_DEBUGUART, 0x14) & (UINT32_C(1) << 5)));
	WriteReg32(IO_CM4_DEBUGUART, 0x0, character);
}

