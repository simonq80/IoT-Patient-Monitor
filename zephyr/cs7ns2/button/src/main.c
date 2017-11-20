#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include <misc/printk.h>

#define LED_PORT		LED0_GPIO_PORT
#define BTN_PORT		SW0_GPIO_NAME
#define LED				LED0_GPIO_PIN
#define BTN				SW0_GPIO_PIN

#define SLEEP_TIME 50

void main(void)
{
	struct device *led_dev;
	struct device *btn_dev;

	u32_t cur_val;
	u32_t last_val = 1;
	u32_t cnt = 0;

	led_dev = device_get_binding(LED_PORT);
	gpio_pin_configure(led_dev, LED, GPIO_DIR_OUT);

	btn_dev = device_get_binding(BTN_PORT);
	gpio_pin_configure(btn_dev, BTN, GPIO_DIR_IN | GPIO_PUD_PULL_UP);

	gpio_pin_write(led_dev, LED, 0);

	while (1) {
		gpio_pin_read(btn_dev, BTN, &cur_val);
		if (cur_val == 0 && last_val == 1) {
			cnt++;
			printk("Button press detected (%d)\n", cnt);
			gpio_pin_write(led_dev, LED, cnt & 1);
		}
		last_val = cur_val;
		k_sleep(SLEEP_TIME);
	}
}
