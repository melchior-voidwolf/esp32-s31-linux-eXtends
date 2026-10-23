// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#pragma once

#include <stdbool.h>

/* The peripherals of the board, before and after the partitions are loaded. */
void board_init(void);
void board_start_display(void);

void init_rmt_led(void);
void init_sd_card(void);
/* Kernel Image from /sd/IMAGE.BIN on the card, false when absent or invalid. */
bool load_kernel_sd(void);
/* The ESP-Hosted firmware: app_main() of external/esp-hosted-linux. */
void esp_hosted_main(void);
