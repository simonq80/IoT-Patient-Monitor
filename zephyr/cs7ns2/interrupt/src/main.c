#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include <misc/printk.h>

#define LED_PORT		LED0_GPIO_PORT
#define BTN_PORT		SW0_GPIO_NAME
#define LED				LED0_GPIO_PIN
#define BTN				SW0_GPIO_PIN

struct device *led_dev;
struct device *btn_dev;
struct gpio_callback btn_callback;

u32_t cnt = 0;

void btn_handler(struct device *port, struct gpio_callback *cb,
					u32_t pins)
{
	cnt++;
	printk("Button pressed (%d)\n", cnt);
	gpio_pin_write(led_dev, LED, cnt & 1);
}

void main(void)
{
	led_dev = device_get_binding(LED_PORT);
	gpio_pin_configure(led_dev, LED, GPIO_DIR_OUT);

	btn_dev = device_get_binding(BTN_PORT);
	gpio_pin_configure(btn_dev, BTN, GPIO_DIR_IN | GPIO_INT
		| GPIO_INT_ACTIVE_LOW | GPIO_INT_EDGE | GPIO_PUD_PULL_UP);

	gpio_init_callback(&btn_callback, btn_handler, BIT(BTN));
	gpio_add_callback(btn_dev, &btn_callback);
	gpio_pin_enable_callback(btn_dev, BTN);

	gpio_pin_write(led_dev, LED, 0);
}
