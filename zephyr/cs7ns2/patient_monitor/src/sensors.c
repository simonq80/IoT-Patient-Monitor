#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include <misc/printk.h>
#include "config.h"
#include "tb_pubsub.h"
#include <math.h>
#include <stdlib.h>

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

u32_t state[4];
uint16_t ls_samples[MAX_SAMPLES];

K_MUTEX_DEFINE(simulate_sensor);
K_MUTEX_DEFINE(bed_occupancy_sensor);

#define BTN_PORT SW0_GPIO_NAME
#define BTN_COUNT 4

const u32_t btn_arr[BTN_COUNT] = {
    SW0_GPIO_PIN,
    SW1_GPIO_PIN,
    SW2_GPIO_PIN,
    SW3_GPIO_PIN
};

struct device *btn_dev;
struct gpio_callback btn_callback;

int btn_alert_handler(struct k_alert *alert);
K_ALERT_DEFINE(btn_alert, btn_alert_handler, 10);

// configure simulator thread
K_THREAD_STACK_DEFINE(ss_stack_area, SIMULATOR_STACK_SIZE);
struct k_thread ss_thread;

// configure bed occupancy sensor (bos) thread
K_THREAD_STACK_DEFINE(bos_stack_area, SIMULATOR_STACK_SIZE);
struct k_thread bos_thread;

//  config the analog to digital converter
void adc_init(){
	*adc_ch1_pselp = AIN1;
	*adc_ch1_config = ADC_REFSEL_VDD_4 | ADC_TACQ_40;
	*adc_enable = 1;
}

// check if the patient is in bed
void fetch_bos_state(void * a, void * b, void * c){

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	char payload[16];

	while (true) {
		k_sleep(BOS_SLEEP_MSECS);

    // obtain a lock
		k_mutex_lock(&bed_occupancy_sensor, K_FOREVER);

		*adc_result_ptr = ls_samples;
		*adc_result_max = MAX_SAMPLES;

		// clear DONE event
		*adc_event_done = 0;

		// trigger START task
		*adc_task_start = 1;

		// trigger SAMPLE task
		*adc_task_sample = 1;

		k_sleep(BOS_PROCESS_TIME);

		*adc_task_stop = 1;

		// check if patient is in bed
		bool is_in_bed;

		if(ls_samples[0] < READING_WHEN_OUT_OF_BED){
			is_in_bed = true;
		} else {
			is_in_bed = false;
		}

		printk("patient in bed ?: %s\n",is_in_bed ? "true" : "false");
		snprintf(payload, sizeof(payload), "{\"occ\":%s}",is_in_bed ? "true" : "false");

		k_mutex_unlock(&bed_occupancy_sensor);

		// publish patient occupany data
		tb_publish_telemetry(payload);
	}

}

// generate fake temperature readings
void emulate_sensors(void * e, void * f, void * g){

	ARG_UNUSED(e);
	ARG_UNUSED(f);
	ARG_UNUSED(g);
	char payload[128];

	while (true) {
		k_sleep(APP_SLEEP_MSECS);

		k_mutex_lock(&simulate_sensor, K_FOREVER);

		int temperature = generate_value_in_range(40, 33);
		int heartbeat = generate_value_in_range(140, 50);

		snprintf(payload, sizeof(payload), "{\"tmp\":\"%i\",\"hrt\":\"%i\"}",temperature,heartbeat);
		k_mutex_unlock(&simulate_sensor);

    // publish simulated sensor data
		tb_publish_telemetry(payload);
	}
}

int generate_value_in_range(int max, int min){
	return rand() % (max + 1 - min) + min;
}

void btn_handler(struct device *port, struct gpio_callback *cb,
					u32_t pins)
{
    /* Context: interrupt handler */

	printk("Signalling alert!\n");
	k_alert_send(&btn_alert);
}

int btn_alert_handler(struct k_alert *alert)
{
	int value;
	char payload[16];

    /* Context: Zephy kernel workqueue thread */

	printk("Button event!\n");

    /* Iterate over each button looking for a change from the previously
     * published state */
	for (u32_t i = 0; i < BTN_COUNT; i++) {
		gpio_pin_read(btn_dev, btn_arr[i], &value);
		if (value != state[i]) {
            /* Formulate JSON in the format expected by thingsboard.io */
			snprintf(payload, sizeof(payload), "{\"btn%d\":%s}", i, value ? "false" : "true");
			tb_publish_telemetry(payload);
			state[i] = value;
		}
	}

	return 0;
}

void sensors_start()
{
	btn_dev = device_get_binding(BTN_PORT);

	for (u32_t i = 0; i < BTN_COUNT; i++) {
		gpio_pin_configure(btn_dev, btn_arr[i], GPIO_DIR_IN | GPIO_INT
			| GPIO_INT_DOUBLE_EDGE | GPIO_INT_EDGE | GPIO_PUD_PULL_UP);
	}

	gpio_init_callback(&btn_callback, btn_handler,
		BIT(btn_arr[0]) | BIT(btn_arr[1]) |
		BIT(btn_arr[2]) | BIT(btn_arr[3])
	);

	gpio_add_callback(btn_dev, &btn_callback);

	for (u32_t i = 0; i < BTN_COUNT; i++) {
		gpio_pin_enable_callback(btn_dev, btn_arr[i]);
		gpio_pin_read(btn_dev, btn_arr[i], &state[i]);
	}

  // start sensor simulator
	k_tid_t ss_tid = k_thread_create(&ss_thread, ss_stack_area,
								 K_THREAD_STACK_SIZEOF(ss_stack_area),
								 emulate_sensors,
								 NULL, NULL, NULL,
								 SIMULATOR_PRIORITY, 0, K_NO_WAIT);

	ARG_UNUSED(ss_tid);

  // start bed occupancy sensor
	printk("Initializing bed occupany sensor (bos)");

	adc_init();

  // start occupancy sensor
  k_tid_t bos_tid = k_thread_create(&bos_thread, bos_stack_area,
  							 K_THREAD_STACK_SIZEOF(bos_stack_area),
  							 fetch_bos_state,
  							 NULL, NULL, NULL,
  							 BOS_PRIORITY, 0, K_NO_WAIT);

  ARG_UNUSED(bos_tid);

}
