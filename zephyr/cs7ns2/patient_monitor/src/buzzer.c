#include <zephyr.h>
#include <board.h>
#include <device.h>
#include <gpio.h>
#include "buzzer.h"
#include "config.h"
#include <misc/printk.h>

#define BUZZER_STACK_SIZE 2048
#define BUZZER_PRIORITY 6
#define GPIO_PORT LED0_GPIO_PORT
#define BUZZER_PIN 11
#define BUZZER_INTERVAL_MSECS 700
#define ON 1
#define OFF 0

// configure buzzer thread
K_THREAD_STACK_DEFINE(buzzer_stack_area, BUZZER_STACK_SIZE);
struct k_thread buzzer_thread;

struct device *buzzer_dev;
k_tid_t buzzer_tid;

struct k_timer timer;

// disarms the buzzer if it sounds too long
void expiry_handler()
{
    printk("\nEXPIRED: Buzzer has sounded too long.");
    disarm_buzzer();
}

K_TIMER_DEFINE(timer, expiry_handler, NULL);

// start buzzer
void activate_buzzer(){
    printk("\nBuzzer activated");
    // wait for the ending of the first timer before starting the second
    k_timer_status_sync(&timer);
    k_timer_start(&timer, K_SECONDS(BUZZER_AUTO_DISARM), 0);
    k_thread_resume(buzzer_tid);
}

// silence buzzer
void disarm_buzzer(){
    printk("\nBuzzer disarmed");
    // suspend the thread and toggle the buzzer to off state
    k_thread_suspend(buzzer_tid);
    gpio_pin_write(buzzer_dev, BUZZER_PIN, OFF);
}

// toggle buzz on & off
void perform_buzz(void * a, void * b, void * c){

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    printk("\nBuzzing");

    int index = 0;

    while (1) {
		/* Set pin to HIGH/LOW every 1 second */
		gpio_pin_write(buzzer_dev, BUZZER_PIN, index % 2);
		index++;
		k_sleep(BUZZER_INTERVAL_MSECS);
	}
}


// configure buzzer
void buzzer_init(){
    printk("\nInitiating buzzer setup");
    buzzer_dev = device_get_binding(GPIO_PORT);
    gpio_pin_configure(buzzer_dev, BUZZER_PIN, GPIO_DIR_OUT);

    // initalise buzzer thread
    buzzer_tid = k_thread_create(&buzzer_thread, buzzer_stack_area,
                               K_THREAD_STACK_SIZEOF(buzzer_stack_area),
                               perform_buzz,
                               NULL, NULL, NULL,
                               BUZZER_PRIORITY, 0, K_NO_WAIT);

    //suspend thread execution (prevents buzzer ringing)
    k_thread_suspend(buzzer_tid);

    ARG_UNUSED(buzzer_tid);
}
