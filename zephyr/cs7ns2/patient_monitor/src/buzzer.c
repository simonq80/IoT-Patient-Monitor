#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include "buzzer.h"

#define GPIO_PORT LED0_GPIO_PORT
#define BUZZER_PIN 11
#define ON 1
#define OFF 0

struct device *buzzer_dev;

void activate_buzzer(){
    printk("-- Buzzer activated --");
    gpio_pin_write(buzzer_dev, BUZZER_PIN, ON);
}

void disarm_buzzer(){
    printk("-- Buzzer disarmed --");
    gpio_pin_write(buzzer_dev, BUZZER_PIN, OFF);
}

void buzzer_init(){
    buzzer_dev = device_get_binding(GPIO_PORT);
    gpio_pin_configure(buzzer_dev, BUZZER_PIN, GPIO_DIR_OUT);
}
