/*
 * ESP32-P4 USB-Serial/JTAG FIFO store repro.
 *
 * The IN-empty ISR stores USB_SERIAL_JTAG.ep1.val.
 * The task sets WR_DONE after every byte.
 * With the USB CDC port left open, this store-faults:
 *   MCAUSE 7, MTVAL 0x500d2000, MEPC inside usj_fifo_write.
 *
 * UART0 is the console. A panic prints the registers and halts.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_intr_alloc.h"
#include "esp_rom_sys.h"
#include "esp_private/periph_ctrl.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/periph_defs.h"

#define RING_SIZE 512

static volatile uint32_t s_line;
static volatile uint32_t s_index;
static uint8_t s_ring[RING_SIZE];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;
static intr_handle_t s_intr;

static int IRAM_ATTR usj_fifo_write(const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (!USB_SERIAL_JTAG.ep1_conf.serial_in_ep_data_free) {
            break;
        }
        s_index = i;
        USB_SERIAL_JTAG.ep1.val = buf[i];
    }
    return (int)i;
}

static void IRAM_ATTR usj_fifo_flush(void)
{
    USB_SERIAL_JTAG.ep1_conf.wr_done = 1;
}

static void IRAM_ATTR usj_isr(void *arg)
{
    uint32_t status = usb_serial_jtag_ll_get_intsts_mask();
    (void)arg;
    if (status & USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY) {
        uint8_t chunk[64];
        int n = 0;
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
        while (n < 64 && s_tail != s_head) {
            chunk[n++] = s_ring[s_tail];
            s_tail = (uint16_t)((s_tail + 1) % RING_SIZE);
        }
        if (n > 0) {
            usj_fifo_write(chunk, (uint32_t)n);
            usj_fifo_flush();
            usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
        } else {
            usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
        }
    }
}

static void usj_hw_init(void)
{
    PERIPH_RCC_ATOMIC() {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
    usb_serial_jtag_ll_phy_set_defaults();
    USB_SERIAL_JTAG.conf0.phy_sel = 0;
    USB_SERIAL_JTAG.conf0.dp_pullup = 1;
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
}

static int make_line(char *dst, uint32_t n)
{
    int len = snprintf(dst, 96,
                       "Log msg ... #%lu .....................................\n",
                       (unsigned long)n);
    if (len < 0 || len >= 96) {
        return 0;
    }
    return len;
}

static void ring_push(uint8_t byte)
{
    uint16_t next;
    for (;;) {
        next = (uint16_t)((s_head + 1) % RING_SIZE);
        if (next != s_tail) {
            s_ring[s_head] = byte;
            s_head = next;
            return;
        }
        vTaskDelay(1);
    }
}

void app_main(void)
{
    esp_chip_info_t chip;
    char line[96];

    esp_chip_info(&chip);
    esp_rom_printf("\r\n[p4-usj] split FIFO repro, rev %d\r\n", chip.revision);
    esp_rom_printf("[p4-usj] Open the USB Serial/JTAG port. Watch this UART for the panic.\r\n");

    usj_hw_init();
    if (esp_intr_alloc(ETS_USB_SERIAL_JTAG_INTR_SOURCE, 0, usj_isr, NULL, &s_intr) != ESP_OK) {
        esp_rom_printf("[p4-usj] interrupt alloc failed\r\n");
        return;
    }

    s_line = 0;
    while (1) {
        int len = make_line(line, s_line);
        int i;
        for (i = 0; i < len; i++) {
            ring_push((uint8_t)line[i]);
            usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
            usj_fifo_flush();
        }
        s_line++;
        if ((s_line % 50) == 0) {
            esp_rom_printf("[p4-usj] lines %lu last index %lu\r\n",
                           (unsigned long)s_line, (unsigned long)s_index);
        }
    }
}
