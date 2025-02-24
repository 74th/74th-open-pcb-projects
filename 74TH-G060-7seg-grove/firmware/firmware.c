#include "ch32v003fun.h"
#include "ch32v003_GPIO_branchless.h"
#include <stdio.h>
#include <stdbool.h>

#define LED_A_PIN PC4  // 7SEG 7
#define LED_B_PIN PC3  // 7SEG 6
#define LED_C_PIN PD3  // 7SEG 4
#define LED_D_PIN PD2  // 7SEG 2
#define LED_E_PIN PC7  // 7SEG 1
#define LED_F_PIN PC5  // 7SEG 9
#define LED_G_PIN PC6  // 7SEG 10
#define LED_DP_PIN PA1 // 7SEG 5
#define SEL1_PIN PD6
#define SEL2_PIN PD5
#define SEL3_PIN PD4
#define SEL4_PIN PC0

#define COMM_LED_PIN PA2

#define SEGPATTERN_0 0b00111111
#define SEGPATTERN_1 0b00000110
#define SEGPATTERN_2 0b01011011
#define SEGPATTERN_3 0b01001111
#define SEGPATTERN_4 0b01100110
#define SEGPATTERN_5 0b01101101
#define SEGPATTERN_6 0b01111101
#define SEGPATTERN_7 0b00000111
#define SEGPATTERN_8 0b01111111
#define SEGPATTERN_9 0b01101111
#define SEGPATTERN_d 0b10000000

#define I2C_ADDRESS 0x73
#define IO_BASE 0x00
#define INT_BASE 0x10
#define TEXT_BASE 0x20

#define MODULE_LED_NUM 8
#define MODULE_NUM 4

#define DEMO_MODE 0

uint16_t LED_PINS[MODULE_LED_NUM] = {LED_A_PIN, LED_B_PIN, LED_C_PIN, LED_D_PIN, LED_E_PIN, LED_F_PIN, LED_G_PIN, LED_DP_PIN};
uint16_t SEL_PINS[MODULE_NUM] = {SEL1_PIN, SEL2_PIN, SEL3_PIN, SEL4_PIN};

volatile uint8_t i2c_registers[32] = {0x00};
volatile uint8_t i2c_registers_prev[32] = {0x00};

typedef struct
{
	uint8_t reg;
	uint8_t length;
} I2CMosiEvent;

void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void) __attribute__((interrupt));

void init_rcc(void)
{
	RCC->CFGR0 &= ~(0x1F << 11);

	RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO;
	RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
}

void init_i2c_slave(uint8_t address)
{
	// https://github.com/cnlohr/ch32v003fun/blob/master/examples/i2c_slave/i2c_slave.h

	// PC1 is SDA, 10MHz Output, alt func, open-drain
	GPIOC->CFGLR &= ~(0xf << (4 * 1));
	GPIOC->CFGLR |= (GPIO_Speed_10MHz | GPIO_CNF_OUT_OD_AF) << (4 * 1);

	// PC2 is SCL, 10MHz Output, alt func, open-drain
	GPIOC->CFGLR &= ~(0xf << (4 * 2));
	GPIOC->CFGLR |= (GPIO_Speed_10MHz | GPIO_CNF_OUT_OD_AF) << (4 * 2);

	// Reset I2C1 to init all regs
	RCC->APB1PRSTR |= RCC_APB1Periph_I2C1;
	RCC->APB1PRSTR &= ~RCC_APB1Periph_I2C1;

	// disable SWD
	// AFIO->PCFR1 |= AFIO_PCFR1_SWJ_CFG_DISABLE;

	I2C1->CTLR1 |= I2C_CTLR1_SWRST;
	I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

	// Set module clock frequency
	uint32_t prerate = 2000000; // I2C Logic clock rate, must be higher than the bus clock rate
	I2C1->CTLR2 |= (FUNCONF_SYSTEM_CORE_CLOCK / prerate) & I2C_CTLR2_FREQ;

	// Enable interrupts
	I2C1->CTLR2 |= I2C_CTLR2_ITBUFEN;
	I2C1->CTLR2 |= I2C_CTLR2_ITEVTEN; // Event interrupt
	I2C1->CTLR2 |= I2C_CTLR2_ITERREN; // Error interrupt

	NVIC_EnableIRQ(I2C1_EV_IRQn); // Event interrupt
	NVIC_SetPriority(I2C1_EV_IRQn, 2 << 4);
	NVIC_EnableIRQ(I2C1_ER_IRQn); // Error interrupt
	// Set clock configuration
	uint32_t clockrate = 1000000;																	 // I2C Bus clock rate, must be lower than the logic clock rate
	I2C1->CKCFGR = ((FUNCONF_SYSTEM_CORE_CLOCK / (3 * clockrate)) & I2C_CKCFGR_CCR) | I2C_CKCFGR_FS; // Fast mode 33% duty cycle
	// I2C1->CKCFGR = ((APB_CLOCK/(25*clockrate))&I2C_CKCFGR_CCR) | I2C_CKCFGR_DUTY | I2C_CKCFGR_FS; // Fast mode 36% duty cycle
	// I2C1->CKCFGR = (APB_CLOCK/(2*clockrate))&I2C_CKCFGR_CCR; // Standard mode good to 100kHz

	// Set I2C address
	I2C1->OADDR1 = address << 1;

	// Enable I2C
	I2C1->CTLR1 |= I2C_CTLR1_PE;

	// Acknowledge the first address match event when it happens
	I2C1->CTLR1 |= I2C_CTLR1_ACK;
}

uint16_t i2c_scan_start_position = 0;
uint16_t i2c_scan_length = 0;
bool i2c_firse_byte = true;
bool i2c_mosi_event = false;
bool on_mosi_event = false;
I2CMosiEvent latest_mosi_event = {0, 0};
bool address2matched = false;

void I2C1_EV_IRQHandler(void)
{
	uint16_t STAR1, STAR2 __attribute__((unused));
	STAR1 = I2C1->STAR1;
	STAR2 = I2C1->STAR2;

	// printf("EV STAR1: 0x%04x STAR2: 0x%04x\r\n", STAR1, STAR2);

	I2C1->CTLR1 |= I2C_CTLR1_ACK;

	if (STAR1 & I2C_STAR1_ADDR) // 0x0002
	{
		// 最初のイベント
		// read でも write でも必ず最初に呼ばれる
		address2matched = !!(STAR2 & I2C_STAR2_DUALF);
		// printf("ADDR %d\r\n", address2matched);
		i2c_firse_byte = true;
		i2c_mosi_event = false;
		i2c_scan_start_position = 0;
		i2c_scan_length = 0;
	}

	if (STAR1 & I2C_STAR1_RXNE) // 0x0040
	{
		if (i2c_firse_byte)
		{
			i2c_scan_start_position = I2C1->DATAR;
			i2c_scan_length = 0;
			i2c_firse_byte = false;
			i2c_mosi_event = true;
			// printf("RXNE write event: first_write 0x%x\r\n", i2c_scan_start_position);
		}
		else
		{
			uint16_t pos = i2c_scan_start_position + i2c_scan_length;
			uint8_t v = I2C1->DATAR;
			// printf("RXNE write event: pos:0x%x v:0x%x\r\n", pos, v);
			if (pos < 32)
			{
				i2c_registers[pos] = v;
				i2c_scan_length++;
			}
		}
	}

	if (STAR1 & I2C_STAR1_TXE) // 0x0080
	{
		if (i2c_firse_byte)
		{
			i2c_scan_start_position = I2C1->DATAR;
			i2c_scan_length = 0;
			i2c_firse_byte = false;
			i2c_mosi_event = true;
			// printf("TXE write event: first_write:%x\r\n", i2c_scan_start_position);
		}
		else
		{
			// 1byte の read イベント（slave -> master）
			// 1byte 送信
			uint16_t pos = i2c_scan_start_position + i2c_scan_length;
			uint8_t v = 0x00;
			if (pos < 32)
			{
				v = i2c_registers[pos];
			}
			// printf("TXE write event: pos:0x%x v:0x%x\r\n", pos, v);
			I2C1->DATAR = v;
			i2c_scan_length++;
		}
	}

	if (STAR1 & I2C_STAR1_STOPF)
	{
		I2C1->CTLR1 &= ~(I2C_CTLR1_STOP);
		if (i2c_mosi_event)
		{
			// printf("on_mosi_event\r\n");
			latest_mosi_event.reg = i2c_scan_start_position;
			latest_mosi_event.length = i2c_scan_length;
			on_mosi_event = true;
		}
	}
}

void I2C1_ER_IRQHandler(void)
{
	uint16_t STAR1 = I2C1->STAR1;

	// printf("ER STAR1: 0x%04x\r\n", STAR1);

	if (STAR1 & I2C_STAR1_BERR)			  // 0x0100
	{									  // Bus error
		I2C1->STAR1 &= ~(I2C_STAR1_BERR); // Clear error
	}

	if (STAR1 & I2C_STAR1_ARLO)			  // 0x0200
	{									  // Arbitration lost error
		I2C1->STAR1 &= ~(I2C_STAR1_ARLO); // Clear error
	}

	if (STAR1 & I2C_STAR1_AF)			// 0x0400
	{									// Acknowledge failure
		I2C1->STAR1 &= ~(I2C_STAR1_AF); // Clear error
	}
}

void print_bits(uint8_t value)
{
	for (int i = 7; i >= 0; i--)
	{
		printf("%d", (value >> i) & 1);
	}
}

#define DURATION_MS 200

void output_led()
{
	for (int m = 0; m < MODULE_NUM; m++)
	{
		// funDigitalWrite(SEL_PINS[m], 0);
		for (int n = 0; n < MODULE_LED_NUM; n++)
		{
			funDigitalWrite(SEL_PINS[n], n != m);
		}

		uint8_t v = i2c_registers[IO_BASE + m];

		for (int l = 0; l < MODULE_LED_NUM; l++)
		{
			uint8_t b = (v >> l);
			funDigitalWrite(LED_PINS[l], ((v >> l) & 1));
		}
		Delay_Us(100);

		for (int l = 0; l < MODULE_LED_NUM; l++)
		{
			uint8_t b = (v >> l);
			funDigitalWrite(LED_PINS[l], 0);
		}

		// funDigitalWrite(SEL_PINS[m], 1);
		for (int n = 0; n < MODULE_LED_NUM; n++)
		{
			funDigitalWrite(SEL_PINS[n], 1);
		}
	}
}

void set_num(uint8_t pos, uint8_t num)
{
	switch (num)
	{
	case 0:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_0;
		break;
	case 1:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_1;
		break;
	case 2:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_2;
		break;
	case 3:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_3;
		break;
	case 4:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_4;
		break;
	case 5:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_5;
		break;
	case 6:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_6;
		break;
	case 7:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_7;
		break;
	case 8:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_8;
		break;
	case 9:
		i2c_registers[IO_BASE + pos] = SEGPATTERN_9;
		break;
	}
}

void update_int()
{
	uint16_t num = i2c_registers[INT_BASE] << 8 | i2c_registers[INT_BASE + 1];
	// printf("update_int %d\r\n", num);

	set_num(0, num % 10);
	if (num > 10)
	{
		set_num(1, (num / 10) % 10);
	}
	else
	{
		i2c_registers[IO_BASE + 1] = 0x00;
	}
	if (num > 100)
	{
		set_num(2, (num / 100) % 10);
	}
	else
	{
		i2c_registers[IO_BASE + 2] = 0x00;
	}
	if (num > 1000)
	{
		set_num(3, num / 1000);
	}
	else
	{
		i2c_registers[IO_BASE + 3] = 0x00;
	}

	uint8_t point = i2c_registers[INT_BASE + 2];
	for (int i = 0; i < 4; i++)
	{
		if (i == point)
		{
			i2c_registers[IO_BASE + i] |= 0x80;
		}
		else
		{
			i2c_registers[IO_BASE + i] &= ~(0x80);
		}
	}
}

uint32_t comm_led_last_tick = 0;

void on_write(uint8_t reg, uint8_t length)
{
	funDigitalWrite(COMM_LED_PIN, 0);

	// printf("do_mosi_event %d:%d\r\n", reg, reg + length);
	for (uint8_t r = reg; r < reg + length; r++)
	{
		switch (reg)
		{
		case INT_BASE:
			update_int();
			break;
		}
	}

	comm_led_last_tick = SysTick->CNT;
}

void setup()
{
	funGpioInitAll();

	init_rcc();
	init_i2c_slave(I2C_ADDRESS);

	for (int m = 0; m < MODULE_NUM; m++)
	{
		GPIO_pinMode(SEL_PINS[m], GPIO_pinMode_O_pushPull, GPIO_Speed_10MHz);
		// funPinMode(LED_PINS[m], GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);
	}
	for (int l = 0; l < MODULE_LED_NUM; l++)
	{
		GPIO_pinMode(LED_PINS[l], GPIO_pinMode_O_pushPull, GPIO_Speed_10MHz);
		// funPinMode(LED_PINS[l], GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);
	}
	GPIO_pinMode(COMM_LED_PIN, GPIO_pinMode_O_pushPull, GPIO_Speed_10MHz);

	Delay_Ms(1);

	for (int m = 0; m < MODULE_NUM; m++)
	{
		funDigitalWrite(SEL_PINS[m], 1);
	}
	for (int l = 0; l < MODULE_LED_NUM; l++)
	{
		funDigitalWrite(SEL_PINS[l], 0);
	}

	i2c_registers[INT_BASE + 2] = 0xFF;

	funDigitalWrite(COMM_LED_PIN, 1);

	Delay_Ms(10);
}

#define STEP_TICK 600 * DELAY_MS_TIME
uint32_t start_tick = 0;

bool is_step_timing()
{
	uint32_t now_tick = SysTick->CNT;
	if (now_tick < start_tick)
	{
		start_tick = 0;
		return;
	}

	if (now_tick - start_tick > STEP_TICK)
	{
		start_tick = now_tick;
		return true;
	}

	return false;
}

void turn_off_comm_led()
{
	uint32_t now_tick = SysTick->CNT;
	if (comm_led_last_tick > 0 && (now_tick < comm_led_last_tick || now_tick - comm_led_last_tick > 10 * DELAY_MS_TIME))
	{
		funDigitalWrite(COMM_LED_PIN, 1);
		comm_led_last_tick = 0;
	}
}

int step = 0;

void demo_mode()
{
	if (!is_step_timing())
	{
		return;
	}

	// printf("demo step: %d\r\n", step);

	if (step < 8)
	{
		i2c_registers[IO_BASE + 0] = 1 << step;
		i2c_registers[IO_BASE + 1] = 1 << step;
		i2c_registers[IO_BASE + 2] = 1 << step;
		i2c_registers[IO_BASE + 3] = 1 << step;
	}
	if (step == 8)
	{
		i2c_registers[IO_BASE + 0] = 0xFF;
		i2c_registers[IO_BASE + 1] = 0x00;
		i2c_registers[IO_BASE + 2] = 0xFF;
		i2c_registers[IO_BASE + 3] = 0x00;
	}
	if (step == 9)
	{
		i2c_registers[IO_BASE + 0] = 0x00;
		i2c_registers[IO_BASE + 1] = 0xFF;
		i2c_registers[IO_BASE + 2] = 0x00;
		i2c_registers[IO_BASE + 3] = 0xFF;
	}
	if (step > 9)
	{
		uint8_t v = 0;
		switch (step - 10)
		{
		case 0:
			v = SEGPATTERN_0;
			break;
		case 1:
			v = SEGPATTERN_1;
			break;
		case 2:
			v = SEGPATTERN_2;
			break;
		case 3:
			v = SEGPATTERN_3;
			break;
		case 4:
			v = SEGPATTERN_4;
			break;
		case 5:
			v = SEGPATTERN_5;
			break;
		case 6:
			v = SEGPATTERN_6;
			break;
		case 7:
			v = SEGPATTERN_7;
			break;
		case 8:
			v = SEGPATTERN_8;
			break;
		case 9:
			v = SEGPATTERN_9;
			break;
		case 10:
			v = SEGPATTERN_d;
			break;
		}

		i2c_registers[IO_BASE + 0] = v;
		i2c_registers[IO_BASE + 1] = v;
		i2c_registers[IO_BASE + 2] = v;
		i2c_registers[IO_BASE + 3] = v;
	}

	// print_bits(i2c_registers[IO_BASE + 0]);

	step++;
	if (step >= 20)
	{
		step = 0;
	}
}

void main_loop()
{

	if (on_mosi_event)
	{
		// printf("do_mosi_event\r\n");
		on_mosi_event = false;
		on_write(latest_mosi_event.reg, latest_mosi_event.length);
		// printf("done_mosi_event\r\n");
	}
	if (DEMO_MODE)
	{
		demo_mode();
	}

	output_led();

	turn_off_comm_led();
}

int main()
{
	SystemInit();

	setup();
	printf("Start!");

	for (;;)
		main_loop();
}
