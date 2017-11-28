#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include <misc/printk.h>

#define AIN1		2
#define MAX_SAMPLES 16

#define ADC_REG_BASE			0x40007000
#define ADC_ENABLE_REG     		(ADC_REG_BASE + 0x500)
#define ADC_TASK_START_REG    	(ADC_REG_BASE + 0x0)
#define ADC_TASK_SAMPLE_REG     (ADC_REG_BASE + 0x4)
#define ADC_TASK_STOP_REG     	(ADC_REG_BASE + 0x8)
#define ADC_EVENT_DONE_REG      (ADC_REG_BASE + 0x108)
#define ADC_CH0_PSELP_REG      	(ADC_REG_BASE + 0x510)
#define ADC_CH0_CONFIG_REG      (ADC_REG_BASE + 0x518)
#define ADC_RESULT_PTR_REG		(ADC_REG_BASE + 0x62C)
#define ADC_RESULT_MAX_REG		(ADC_REG_BASE + 0x630)
#define ADC_RESULT_CNT_REG		(ADC_REG_BASE + 0x634)

volatile uint32_t * const adc_enable = ADC_ENABLE_REG;
volatile uint32_t * const adc_task_start = ADC_TASK_START_REG;
volatile uint32_t * const adc_task_sample = ADC_TASK_SAMPLE_REG;
volatile uint32_t * const adc_task_stop = ADC_TASK_STOP_REG;
volatile uint32_t * const adc_event_done = ADC_EVENT_DONE_REG;
volatile uint32_t * const adc_ch1_pselp = ADC_CH0_PSELP_REG;
volatile uint32_t * const adc_ch1_config = ADC_CH0_CONFIG_REG;
volatile uint32_t * const adc_result_ptr = ADC_RESULT_PTR_REG;
volatile uint32_t * const adc_result_max = ADC_RESULT_MAX_REG;
volatile uint32_t * const adc_result_cnt = ADC_RESULT_CNT_REG;

#define ADC_REFSEL_VDD_4		(1 << 12)
#define ADC_TACQ_40				(5 << 16)


uint16_t samples[MAX_SAMPLES];

void main(void)
{
	printk("Preparing ADC\n");

	*adc_ch1_pselp = AIN1;
	*adc_ch1_config = ADC_REFSEL_VDD_4 | ADC_TACQ_40;
	*adc_enable = 1;

	while (true) {
		k_sleep(6000);

		printk("\nSampling ... ");

		*adc_result_ptr = samples;
		*adc_result_max = MAX_SAMPLES;

		// clear DONE event
		*adc_event_done = 0;

		// trigger START task
		*adc_task_start = 1;

		// trigger SAMPLE task
		*adc_task_sample = 1;

		// delay at least 40uS
		k_sleep(100);

		*adc_task_stop = 1;

		// read from RAM at RESULT_PTR
		printk("stored: %d, value: %d\n", *adc_result_cnt, samples[0]);
	}
}
