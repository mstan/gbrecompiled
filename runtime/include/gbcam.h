/**
 * @file gbcam.h
 * @brief Game Boy Camera webcam capture — cross-platform abstraction
 *
 * Provides webcam capture for the Pocket Camera (MBC type 0xFC).
 * Platform backends: V4L2 (Linux), AVFoundation (macOS), Media Foundation (Windows).
 */

#ifndef GBCAM_H
#define GBCAM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GB Camera sensor image dimensions */
#define GBCAM_WIDTH   128
#define GBCAM_HEIGHT  112
#define GBCAM_TILES_X 16
#define GBCAM_TILES_Y 14
#define GBCAM_TILE_COUNT (GBCAM_TILES_X * GBCAM_TILES_Y)
#define GBCAM_IMG_OFFSET 0x0100
#define GBCAM_IMG_SIZE   (GBCAM_TILE_COUNT * 16)

/**
 * Camera device descriptor for programmatic enumeration.
 *   path  - what to pass to gbcam_open (e.g. "/dev/video0" on Linux,
 *           the index string "0" on macOS/Windows). Empty if unused.
 *   label - human-readable name (cap.card on Linux, AVCaptureDevice
 *           localizedName on macOS, friendlyName on Windows).
 */
typedef struct {
    char path[64];
    char label[96];
} GBCamDevice;

/**
 * List available video capture devices to stdout (diagnostic).
 */
void gbcam_list_devices(void);

/**
 * Programmatic enumeration. Fills up to `max_count` entries into `out`
 * and returns the number written. Pass NULL/0 to just probe the count.
 */
int gbcam_enumerate_devices(GBCamDevice* out, int max_count);

/**
 * Return the device path most recently passed to gbcam_open, or "" if
 * the camera has never been opened.
 */
const char* gbcam_current_device(void);

/**
 * Open a webcam device. Pass NULL to use GBCAM_DEVICE env var or platform default.
 * Returns true on success. Safe to call multiple times (no-op if already open).
 */
bool gbcam_open(const char* device);

/**
 * Close the webcam and free resources.
 */
void gbcam_close(void);

/**
 * Grab the latest available frame as 128x112 grayscale (non-blocking).
 * Returns true if a new frame was written to `grayscale_out`.
 * If no new frame is ready, returns false (caller should reuse cached data).
 * Buffer must be at least GBCAM_WIDTH * GBCAM_HEIGHT bytes.
 */
bool gbcam_grab_grayscale(uint8_t* grayscale_out);

/**
 * Full capture pipeline: grab webcam frame, apply image processing
 * (edge enhancement, auto-levels, brightness/contrast, Floyd-Steinberg dithering),
 * and write 2bpp GB tile data to the provided SRAM buffer.
 *
 * @param sram       Pointer to SRAM bank 0 (must be >= GBCAM_IMG_OFFSET + GBCAM_IMG_SIZE)
 * @param sram_size  Size of SRAM buffer
 * @param cam_regs   Camera sensor registers 0-5 (written by the game)
 * @return true if image was captured and written
 */
bool gbcam_capture_to_sram(uint8_t* sram, uint32_t sram_size, const uint8_t cam_regs[6]);

#ifdef __cplusplus
}
#endif

#endif /* GBCAM_H */
