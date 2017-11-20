#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include <misc/printk.h>
#include "config.h"
#include "tb_pubsub.h"

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

u32_t state[4];

int btn_alert_handler(struct k_alert *alert);
K_ALERT_DEFINE(btn_alert, btn_alert_handler, 10);

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
}
