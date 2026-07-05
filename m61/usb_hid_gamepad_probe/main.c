#include <FreeRTOS.h>
#include "task.h"

#include "board.h"
#include "bflb_mtimer.h"

#if defined(BOARD_USB_VIA_GPIO)
#include "board_gpio.h"
#endif

#define HID_GAMEPAD_STACK_SIZE 1536
#define HID_GAMEPAD_TASK_PRIORITY 16

void hid_gamepad_probe_init(uint8_t busid, uintptr_t reg_base);
void hid_gamepad_probe_poll(uint8_t busid);

static void hid_gamepad_task(void *params)
{
    (void)params;

    hid_gamepad_probe_init(0, 0);

    while (1) {
        hid_gamepad_probe_poll(0);
        bflb_mtimer_delay_ms(10);
    }
}

int main(void)
{
    board_init();

#if defined(BOARD_USB_VIA_GPIO)
    board_usb_gpio_init();
#endif

    xTaskCreate(hid_gamepad_task, "hid_gamepad", HID_GAMEPAD_STACK_SIZE, NULL, HID_GAMEPAD_TASK_PRIORITY, NULL);
    vTaskStartScheduler();

    while (1) {
    }
}
