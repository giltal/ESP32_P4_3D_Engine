/*
 * The Guition JC1060P470C's 1024x600 JD9165 MIPI-DSI panel, showing a small
 * RGB565 frame scaled up by the P4's PPA (a 400x240 frame fills 1000x600).
 *
 *   display_init(400, 240);
 *   for (;;) {
 *       draw(fb[cur]);
 *       display_present(fb[cur]);   // returns at once; the PPA scales it on a task
 *       cur ^= 1;                   // draw the next frame into the other buffer
 *   }
 *
 * display_present waits for the previous frame's scaling first, so once it
 * returns, the buffer passed the time before is free to draw into again.
 * Frame buffers: display_alloc_frame (PSRAM, aligned for the PPA's DMA).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t display_init(int frame_w, int frame_h);
uint16_t *display_alloc_frame(void);
void      display_present(const uint16_t *frame);
