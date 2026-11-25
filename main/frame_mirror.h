#pragma once

#include <stdint.h>

// Mirror a packed 4-bit frame buffer left to right in place, in the panel's
// native orientation. Each row is (width + 1) / 2 bytes, the even pixel in the
// high nibble -- the layout GUI_Paint and the splash screen draw into on every
// board. Applying it twice restores the buffer.
//
// For panels that scan each row the other way from the board they are fitted
// to (#161: an ES103TC1 on a XIAO EE03 built for the ED103TC2), so the picture
// comes out mirrored. Kept free of ESP-IDF headers so it is host-tested.
void frame_mirror_x(uint8_t *buf, int width, int height);
