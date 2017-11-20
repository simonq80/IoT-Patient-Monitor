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

#define BTN_STACK_SIZE 1024
#define BTN_PRIORITY 5
K_THREAD_STACK_DEFINE(btn_stack_area, BTN_STACK_SIZE);
struct k_thread btn_thread;

K_SEM_DEFINE(btn_sem, 0, 1);

void btn_handler(struct device *port, struct gpio_callback *cb, u32_t pins)
{
	printk("Giving semaphore!!");
	k_sem_give(&btn_sem);
}

void process_buttons(void * unused1, void * unused2, void * unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {

		printk("Attempting to take semaphore ...");
		if (k_sem_take(&btn_sem, K_FOREVER) != 0) {
			continue;
		}

		printk("... success!!");

		cnt++;
		printk("Button pressed! (%d)", cnt);
		gpio_pin_write(led_dev, LED, cnt & 1);
	}
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

	k_tid_t ss_tid = k_thread_create(&btn_thread, btn_stack_area,
								 K_THREAD_STACK_SIZEOF(btn_stack_area),
								 process_buttons,
								 NULL, NULL, NULL,
								 BTN_PRIORITY, 0, K_NO_WAIT);
}
