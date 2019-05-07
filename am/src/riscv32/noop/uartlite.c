#include <riscv32.h>

#define UARTLITE_MMIO 0x40600000
#define UARTLITE_RX_FIFO  0x0
#define UARTLITE_TX_FIFO  0x4
#define UARTLITE_STAT_REG 0x8
#define UARTLITE_CTRL_REG 0xc

#define UARTLITE_RST_FIFO 0x03
#define UARTLITE_TX_FULL  0x08

void init_uartlite(void) {
  outb(UARTLITE_MMIO + UARTLITE_CTRL_REG, UARTLITE_RST_FIFO);
}

void uartlite_putchar(char ch) {
  if (ch == '\n') uartlite_putchar('\r');

  while (inb(UARTLITE_MMIO + UARTLITE_STAT_REG) & UARTLITE_TX_FULL);
  outb(UARTLITE_MMIO + UARTLITE_TX_FIFO, ch);
}