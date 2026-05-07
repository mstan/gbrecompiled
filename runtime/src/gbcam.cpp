/**
 * @file gbcam.cpp
 * @brief Game Boy Camera webcam capture — OpenCV cross-platform backend + image processing
 */

#include "gbcam.h"
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ============================================================================
 * OpenCV webcam backend (Linux, macOS, Windows)
 * ========================================================================== */

static cv::VideoCapture* cap = nullptr;
static int cam_state = 0; /* 0=not tried, 1=failed, 2=open */

extern "C" void gbcam_list_devices(void) {
    printf("[GBCAM] Checking available cameras...\n");
    for (int i = 0; i < 10; i++) {
        cv::VideoCapture test(i);
        if (test.isOpened()) {
            int w = (int)test.get(cv::CAP_PROP_FRAME_WIDTH);
            int h = (int)test.get(cv::CAP_PROP_FRAME_HEIGHT);
            auto backend = test.getBackendName();
            printf("[GBCAM]   %d - %dx%d (%s)\n", i, w, h, backend.c_str());
            test.release();
        }
    }
    printf("[GBCAM] Set GBCAM_DEVICE=N to choose a camera (default: 0)\n");
}

extern "C" void gbcam_close(void) {
    if (cap) {
        cap->release();
        delete cap;
        cap = nullptr;
    }
    cam_state = 1;
}

extern "C" bool gbcam_open(const char* device) {
    if (cam_state == 1) return false;
    if (cam_state == 2) return true;

    if (!device) device = getenv("GBCAM_DEVICE");
    int dev_index = device ? atoi(device) : 0;

    gbcam_list_devices();

    cap = new cv::VideoCapture(dev_index);
    if (!cap->isOpened()) {
        printf("[GBCAM] Failed to open camera %d\n", dev_index);
        delete cap;
        cap = nullptr;
        cam_state = 1;
        return false;
    }

    /* Request a small resolution for speed */
    cap->set(cv::CAP_PROP_FRAME_WIDTH, 320);
    cap->set(cv::CAP_PROP_FRAME_HEIGHT, 240);

    int w = (int)cap->get(cv::CAP_PROP_FRAME_WIDTH);
    int h = (int)cap->get(cv::CAP_PROP_FRAME_HEIGHT);
    printf("[GBCAM] Webcam opened: %dx%d (device %d)\n", w, h, dev_index);

    cam_state = 2;
    return true;
}

extern "C" bool gbcam_grab_grayscale(uint8_t* grayscale_out) {
    if (!cap || cam_state != 2) return false;

    cv::Mat frame;
    if (!cap->grab()) return false;
    if (!cap->retrieve(frame)) return false;
    if (frame.empty()) return false;

    /* Convert to grayscale and resize to 128x112 */
    cv::Mat gray, resized;
    if (frame.channels() > 1)
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    else
        gray = frame;

    cv::resize(gray, resized, cv::Size(GBCAM_WIDTH, GBCAM_HEIGHT), 0, 0, cv::INTER_AREA);

    /* Copy to output buffer */
    for (int y = 0; y < GBCAM_HEIGHT; y++) {
        memcpy(grayscale_out + y * GBCAM_WIDTH, resized.ptr(y), GBCAM_WIDTH);
    }

    return true;
}


/* ============================================================================
 * Common image processing pipeline
 * ========================================================================== */

static uint8_t tile_cache[GBCAM_TILE_COUNT * 16];
static bool cache_valid = false;
static uint32_t capture_count = 0;

extern "C" bool gbcam_capture_to_sram(uint8_t* sram, uint32_t sram_size, const uint8_t cam_regs[6]) {
    capture_count++;

    /* Skip boot-time hardware check */
    if (capture_count <= 1) return false;

    if (!sram || sram_size < GBCAM_IMG_OFFSET + GBCAM_IMG_SIZE) return false;

    /* Lazy-open on second capture */
    if (capture_count == 2) {
        if (!gbcam_open(nullptr)) return false;
    }

    uint8_t grayscale[GBCAM_WIDTH * GBCAM_HEIGHT];
    bool got_frame = gbcam_grab_grayscale(grayscale);

    if (got_frame) {
        int total = GBCAM_WIDTH * GBCAM_HEIGHT;

        /* ---- Edge enhancement (unsharp mask like the real sensor) ---- */
        int16_t sharp[GBCAM_WIDTH * GBCAM_HEIGHT];
        for (int y = 0; y < GBCAM_HEIGHT; y++) {
            for (int x = 0; x < GBCAM_WIDTH; x++) {
                int idx = y * GBCAM_WIDTH + x;
                if (x > 0 && x < GBCAM_WIDTH-1 && y > 0 && y < GBCAM_HEIGHT-1) {
                    int blur = (
                        grayscale[idx - GBCAM_WIDTH - 1] + grayscale[idx - GBCAM_WIDTH] * 2 + grayscale[idx - GBCAM_WIDTH + 1] +
                        grayscale[idx - 1] * 2            + grayscale[idx] * 4               + grayscale[idx + 1] * 2 +
                        grayscale[idx + GBCAM_WIDTH - 1]  + grayscale[idx + GBCAM_WIDTH] * 2 + grayscale[idx + GBCAM_WIDTH + 1]
                    ) / 16;
                    int edge = grayscale[idx] - blur;
                    sharp[idx] = grayscale[idx] + (edge * 3 / 5);
                } else {
                    sharp[idx] = grayscale[idx];
                }
                if (sharp[idx] < 0) sharp[idx] = 0;
                if (sharp[idx] > 255) sharp[idx] = 255;
            }
        }

        /* ---- Auto-levels (2nd/98th percentile) ---- */
        int lum_min, lum_max;
        {
            uint32_t hist[256] = {0};
            for (int i = 0; i < total; i++) hist[sharp[i]]++;
            int cutoff = total / 50;
            int count = 0;
            lum_min = 0;
            for (int i = 0; i < 256; i++) {
                count += hist[i];
                if (count >= cutoff) { lum_min = i; break; }
            }
            count = 0;
            lum_max = 255;
            for (int i = 255; i >= 0; i--) {
                count += hist[i];
                if (count >= cutoff) { lum_max = i; break; }
            }
        }

        /* ---- Brightness (exposure register 2-3 shifts midpoint) ---- */
        uint16_t exposure = ((uint16_t)cam_regs[2] << 8) | cam_regs[3];
        int brightness = ((int)exposure - 0x8000) * 80 / 0x8000;
        lum_min -= brightness;
        lum_max -= brightness;

        /* ---- Contrast (register 4 bits 4-6 tighten range) ---- */
        int contrast_level = (cam_regs[4] >> 4) & 0x07;
        {
            int mid = (lum_min + lum_max) / 2;
            int half_range = (lum_max - lum_min) / 2;
            half_range = half_range * (100 - contrast_level * 6) / 100;
            if (half_range < 8) half_range = 8;
            lum_min = mid - half_range;
            lum_max = mid + half_range;
        }

        if (lum_min < 0) lum_min = 0;
        if (lum_max > 255) lum_max = 255;
        if (lum_max <= lum_min) { lum_min = 0; lum_max = 255; }
        int range = lum_max - lum_min;
        if (range == 0) range = 1;

        /* ---- Floyd-Steinberg error diffusion dithering ---- */
        int16_t* diffbuf = new int16_t[total];

        for (int i = 0; i < total; i++) {
            int val = sharp[i];
            if (val <= lum_min) val = 0;
            else if (val >= lum_max) val = 255;
            else val = (val - lum_min) * 255 / range;
            /* Gamma ~1.8 for better midtone separation */
            int sq = val * val / 255;
            val = (val * 2 + sq) / 3;
            diffbuf[i] = (int16_t)val;
        }

        uint8_t shades[GBCAM_WIDTH * GBCAM_HEIGHT];
        for (int y = 0; y < GBCAM_HEIGHT; y++) {
            for (int x = 0; x < GBCAM_WIDTH; x++) {
                int idx = y * GBCAM_WIDTH + x;
                int old_val = diffbuf[idx];
                if (old_val < 0) old_val = 0;
                if (old_val > 255) old_val = 255;

                int shade, new_val;
                if (old_val < 43)       { shade = 3; new_val = 0; }
                else if (old_val < 128) { shade = 2; new_val = 85; }
                else if (old_val < 213) { shade = 1; new_val = 170; }
                else                    { shade = 0; new_val = 255; }
                shades[idx] = (uint8_t)shade;

                int err = old_val - new_val;
                if (x + 1 < GBCAM_WIDTH)
                    diffbuf[idx + 1]                   += err * 7 / 16;
                if (y + 1 < GBCAM_HEIGHT) {
                    if (x > 0)
                        diffbuf[idx + GBCAM_WIDTH - 1] += err * 3 / 16;
                    diffbuf[idx + GBCAM_WIDTH]         += err * 5 / 16;
                    if (x + 1 < GBCAM_WIDTH)
                        diffbuf[idx + GBCAM_WIDTH + 1] += err * 1 / 16;
                }
            }
        }
        delete[] diffbuf;

        /* ---- Pack into 2bpp GB tile format ---- */
        uint8_t* dest = tile_cache;
        for (int ty = 0; ty < GBCAM_TILES_Y; ty++) {
            for (int tx = 0; tx < GBCAM_TILES_X; tx++) {
                for (int row = 0; row < 8; row++) {
                    int py = ty * 8 + row;
                    uint8_t lo = 0, hi = 0;
                    for (int col = 0; col < 8; col++) {
                        int px = tx * 8 + col;
                        uint8_t shade = shades[py * GBCAM_WIDTH + px];
                        lo |= ((shade & 1) << (7 - col));
                        hi |= (((shade >> 1) & 1) << (7 - col));
                    }
                    *dest++ = lo;
                    *dest++ = hi;
                }
            }
        }
        cache_valid = true;
    }

    if (cache_valid) {
        memcpy(sram + GBCAM_IMG_OFFSET, tile_cache, GBCAM_IMG_SIZE);
        return true;
    }
    return false;
}
