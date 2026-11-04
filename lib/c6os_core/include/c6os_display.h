#ifndef C6OS_DISPLAY_H
#define C6OS_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {
namespace display {

constexpr int16_t FB_WIDTH  = 320;
constexpr int16_t FB_HEIGHT = 172;
constexpr int16_t DMA_STRIP_ROWS = 8;   // 2 x 320 x 8 x 2B = 10,240 B dual-DMA buffer

// Returns true if horizontal strip [y0, y1) intersects any region marked in dirty_mask
bool stripOverlapsDirty(uint8_t dirty_mask, int16_t y0, int16_t y1);

// Renders horizontal scanline band [y0, y0 + strip_h) of the active UI screen + overlays
// into out_rgb565 (length >= FB_WIDTH * strip_h). When big_endian_out is true, each 16-bit
// RGB565 pixel is byte-swapped for direct ST7789 SPI2 DMA transmission.
void renderStrip(const RenderSnapshot& snap, int16_t y0, int16_t strip_h,
                 uint16_t* out_rgb565, bool big_endian_out);

// Renders a full 320x172 RGB565 frame strip-by-strip and writes either a 24-bit uncompressed
// BMP (if abs_path ends with ".bmp") or binary P6 PPM image to abs_path via VirtualFs.
bool writeFramebufferImage(SystemState& state, VirtualFs& vfs,
                           const char* abs_path, size_t* out_bytes);

}  // namespace display
}  // namespace c6os

#endif  // C6OS_DISPLAY_H
