#include <stdbool.h>

/* The peripherals of the board, before and after the partitions are loaded. */
void board_init(void);
void board_start_display(void);

/* Kernel Image from /sd/IMAGE.BIN on the card, false when absent or invalid. */
bool load_kernel_sd(void);
