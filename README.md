# ESP32-P4 USB-Serial/JTAG FIFO store

Minimal IDF 6.1 repro of the phase-4 fault. The IN-empty ISR stores `USB_SERIAL_JTAG.ep1.val`. The task sets `WR_DONE` after every byte. It loops until the store faults, then the panic handler prints the registers and halts.

`idf.py build flash monitor` uses the USB Serial/JTAG port. That session is the build, the flash, and the monitor. The monitor prints, then stops, with no panic text. UART0 (GPIO 37/38) is the console. The panic register dump is only on UART0. Open it in a second terminal before the run.

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

A matching panic is a store-access fault, `MTVAL` `0x500d2000`, `MEPC` inside `usj_fifo_write`. Reset the board to run it again.

The capture and the fault analysis are in [ISSUE.md](ISSUE.md).
