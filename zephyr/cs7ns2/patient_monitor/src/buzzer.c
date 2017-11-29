#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include "buzzer.h"
#include <misc/printk.h>

#define BUZZER_STACK_SIZE 2048
#define GPIO_PORT LED0_GPIO_PORT
#define BUZZER_PIN 11
#define BUZZER_INTERVAL_MSECS 700
#define ON 1
#define OFF 0

struct device *buzzer_dev;

// start the buzzer
void activate_buzzer(){
    printk("\nBuzzer activated\n");
    int index = 0;

    while (1) {
		/* Set pin to HIGH/LOW every 1 second */
		gpio_pin_write(buzzer_dev, BUZZER_PIN, index % 2);
		index++;
		k_sleep(BUZZER_INTERVAL_MSECS);
	}
}

// silence buzzer
void disarm_buzzer(){
    printk("\nBuzzer disarmed\n");
    gpio_pin_write(buzzer_dev, BUZZER_PIN, OFF);
}

// configure buzzer
void buzzer_init(){
    buzzer_dev = device_get_binding(GPIO_PORT);
    gpio_pin_configure(buzzer_dev, BUZZER_PIN, GPIO_DIR_OUT);
}
