#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include "buzzer.h"
#include "config.h"
#include <misc/printk.h>

#define BUZZER_PRIORITY 6
#define GPIO_PORT LED0_GPIO_PORT
#define BUZZER_PIN 11
#define BUZZER_INTERVAL_MSECS 700
#define ON 1
#define OFF 0

struct device *buzzer_dev;
struct k_timer timer;

// disarms the buzzer if it sounds too long
void expiry_handler()
{
    printk("\nEXPIRED: Buzzer has sounded too long.");
    disarm_buzzer();
}

// define an expiry timer to automatically expire the buzzer
K_TIMER_DEFINE(timer, expiry_handler, NULL);

// start buzzer
void activate_buzzer(){
    printk("\nBuzzer activated");
    // wait for the ending of the first timer before starting the second
    k_timer_status_sync(&timer);
    gpio_pin_write(buzzer_dev, BUZZER_PIN, ON);
    k_timer_start(&timer, K_SECONDS(BUZZER_AUTO_DISARM), 0);
}

// silence buzzer
void disarm_buzzer(){
    printk("\nBuzzer disarmed");
    gpio_pin_write(buzzer_dev, BUZZER_PIN, OFF);
}

// configure buzzer
void buzzer_init(){
    printk("\nInitiating buzzer setup");
    buzzer_dev = device_get_binding(GPIO_PORT);
    gpio_pin_configure(buzzer_dev, BUZZER_PIN, GPIO_DIR_OUT);
}
