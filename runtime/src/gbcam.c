/**
 * @file gbcam.c
 * @brief Game Boy Camera webcam capture — platform backends + image processing
 *
 * Platform backends: V4L2 (Linux), Media Foundation (Windows).
 * macOS backend is in gbcam_macos.m (Objective-C required for AVFoundation).
 */

#include "gbcam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Platform backend: Linux (V4L2)
 * ========================================================================== */
#if defined(__linux__)

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int v4l2_fd = -2;    /* -2 = not attempted, -1 = failed, >=0 = open */
static void* v4l2_buffer = NULL;
static uint32_t v4l2_buflen = 0;
static uint32_t v4l2_width = 0;
static uint32_t v4l2_height = 0;

void gbcam_list_devices(void) {
    printf("[GBCAM] Available video devices:\n");
    for (int i = 0; i < 10; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR);
        if (fd < 0) continue;
        struct v4l2_capability cap = {0};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
                printf("[GBCAM]   %s - %s (%s)\n", path, cap.card, cap.driver);
            }
        }
        close(fd);
    }
    printf("[GBCAM] Set GBCAM_DEVICE=/dev/videoN to choose a camera\n");
}

void gbcam_close(void) {
    if (v4l2_buffer && v4l2_buffer != MAP_FAILED) {
        munmap(v4l2_buffer, v4l2_buflen);
        v4l2_buffer = NULL;
    }
    if (v4l2_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
        close(v4l2_fd);
    }
    v4l2_fd = -1;
}

bool gbcam_open(const char* device) {
    if (v4l2_fd == -1) return false;
    if (v4l2_fd >= 0) return true;

    if (!device) device = getenv("GBCAM_DEVICE");
    if (!device) device = "/dev/video0";

    gbcam_list_devices();

    v4l2_fd = open(device, O_RDWR | O_NONBLOCK);
    if (v4l2_fd < 0) {
        printf("[GBCAM] Failed to open %s\n", device);
        v4l2_fd = -1;
        return false;
    }
    printf("[GBCAM] Using %s\n", device);

    /* Request YUYV at a small resolution */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 320;
    fmt.fmt.pix.height = 240;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("[GBCAM] Failed to set webcam format\n");
        gbcam_close();
        return false;
    }
    v4l2_width = fmt.fmt.pix.width;
    v4l2_height = fmt.fmt.pix.height;
    printf("[GBCAM] Webcam opened: %ux%u\n", v4l2_width, v4l2_height);

    /* Request one mmap buffer */
    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
        printf("[GBCAM] Failed to request webcam buffers\n");
        gbcam_close();
        return false;
    }

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        printf("[GBCAM] Failed to query webcam buffer\n");
        gbcam_close();
        return false;
    }

    v4l2_buflen = buf.length;
    v4l2_buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, v4l2_fd, buf.m.offset);
    if (v4l2_buffer == MAP_FAILED) {
        printf("[GBCAM] Failed to mmap webcam buffer\n");
        gbcam_close();
        return false;
    }

    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
        printf("[GBCAM] Failed to queue webcam buffer\n");
        gbcam_close();
        return false;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        printf("[GBCAM] Failed to start webcam stream\n");
        gbcam_close();
        return false;
    }

    /* Prime the stream with a short blocking wait just once */
    struct pollfd pfd = { .fd = v4l2_fd, .events = POLLIN };
    poll(&pfd, 1, 300);

    printf("[GBCAM] Webcam streaming (non-blocking)\n");
    return true;
}

bool gbcam_grab_grayscale(uint8_t* grayscale_out) {
    if (v4l2_fd < 0) return false;

    /* Non-blocking dequeue */
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
        return false; /* EAGAIN = no frame ready */
    }

    /* Extract Y (luminance) from YUYV, downsample to 128x112 */
    const uint8_t* yuyv = (const uint8_t*)v4l2_buffer;
    for (int gy = 0; gy < GBCAM_HEIGHT; gy++) {
        int sy = (int)((uint64_t)gy * v4l2_height / GBCAM_HEIGHT);
        for (int gx = 0; gx < GBCAM_WIDTH; gx++) {
            int sx = (int)((uint64_t)gx * v4l2_width / GBCAM_WIDTH);
            uint32_t offset = (uint32_t)(sy * v4l2_width + sx) * 2;
            grayscale_out[gy * GBCAM_WIDTH + gx] =
                (offset < buf.bytesused) ? yuyv[offset] : 0;
        }
    }

    /* Re-queue immediately */
    ioctl(v4l2_fd, VIDIOC_QBUF, &buf);
    return true;
}

/* ============================================================================
 * Platform backend: macOS (AVFoundation) — stub, real impl in gbcam_macos.m
 * ========================================================================== */
#elif defined(__APPLE__)

/* Forward declarations — implemented in gbcam_macos.m */
/* If gbcam_macos.m is not linked, these weak stubs are used. */
void gbcam_list_devices(void) __attribute__((weak));
bool gbcam_open(const char* device) __attribute__((weak));
void gbcam_close(void) __attribute__((weak));
bool gbcam_grab_grayscale(uint8_t* grayscale_out) __attribute__((weak));

void gbcam_list_devices(void) {
    printf("[GBCAM] macOS camera: use GBCAM_DEVICE=N (device index) or omit for default\n");
}

bool gbcam_open(const char* device) {
    (void)device;
    printf("[GBCAM] macOS AVFoundation backend not linked (need gbcam_macos.m)\n");
    return false;
}

void gbcam_close(void) {}

bool gbcam_grab_grayscale(uint8_t* grayscale_out) {
    (void)grayscale_out;
    return false;
}

/* ============================================================================
 * Platform backend: Windows (Media Foundation)
 * ========================================================================== */
#elif defined(_WIN32)

#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

static IMFSourceReader* mf_reader = NULL;
static uint32_t mf_width = 0;
static uint32_t mf_height = 0;
static bool mf_initialized = false;
static int mf_state = 0; /* 0=not tried, 1=failed, 2=open */

void gbcam_list_devices(void) {
    if (!mf_initialized) {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        mf_initialized = true;
    }

    IMFAttributes* attrs = NULL;
    MFCreateAttributes(&attrs, 1);
    IMFAttributes_SetGUID(attrs, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                          &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = NULL;
    UINT32 count = 0;
    MFEnumDeviceSources(attrs, &devices, &count);

    printf("[GBCAM] Available video devices:\n");
    for (UINT32 i = 0; i < count; i++) {
        WCHAR* name = NULL;
        UINT32 name_len = 0;
        IMFActivate_GetAllocatedString(devices[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                        &name, &name_len);
        if (name) {
            printf("[GBCAM]   %u - %ls\n", i, name);
            CoTaskMemFree(name);
        }
        IMFActivate_Release(devices[i]);
    }
    if (devices) CoTaskMemFree(devices);
    if (attrs) IMFAttributes_Release(attrs);

    printf("[GBCAM] Set GBCAM_DEVICE=N to choose a camera (default: 0)\n");
}

void gbcam_close(void) {
    if (mf_reader) {
        IMFSourceReader_Release(mf_reader);
        mf_reader = NULL;
    }
    mf_state = 1;
}

bool gbcam_open(const char* device) {
    if (mf_state == 1) return false;
    if (mf_state == 2) return true;

    if (!mf_initialized) {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        mf_initialized = true;
    }

    gbcam_list_devices();

    int dev_index = 0;
    if (!device) device = getenv("GBCAM_DEVICE");
    if (device) dev_index = atoi(device);

    /* Enumerate and activate the selected device */
    IMFAttributes* attrs = NULL;
    MFCreateAttributes(&attrs, 1);
    IMFAttributes_SetGUID(attrs, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                          &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = NULL;
    UINT32 count = 0;
    MFEnumDeviceSources(attrs, &devices, &count);

    if ((UINT32)dev_index >= count) {
        printf("[GBCAM] Device index %d not found (%u available)\n", dev_index, count);
        for (UINT32 i = 0; i < count; i++) IMFActivate_Release(devices[i]);
        if (devices) CoTaskMemFree(devices);
        IMFAttributes_Release(attrs);
        mf_state = 1;
        return false;
    }

    IMFMediaSource* source = NULL;
    HRESULT hr = IMFActivate_ActivateObject(devices[dev_index], &IID_IMFMediaSource, (void**)&source);
    for (UINT32 i = 0; i < count; i++) IMFActivate_Release(devices[i]);
    CoTaskMemFree(devices);
    IMFAttributes_Release(attrs);

    if (FAILED(hr) || !source) {
        printf("[GBCAM] Failed to activate device %d\n", dev_index);
        mf_state = 1;
        return false;
    }

    /* Create source reader */
    IMFAttributes* reader_attrs = NULL;
    MFCreateAttributes(&reader_attrs, 1);
    hr = MFCreateSourceReaderFromMediaSource(source, reader_attrs, &mf_reader);
    IMFMediaSource_Release(source);
    if (reader_attrs) IMFAttributes_Release(reader_attrs);

    if (FAILED(hr) || !mf_reader) {
        printf("[GBCAM] Failed to create source reader\n");
        mf_state = 1;
        return false;
    }

    /* Request NV12 or YUY2 output */
    IMFMediaType* media_type = NULL;
    MFCreateMediaType(&media_type);
    IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_YUY2);
    IMFSourceReader_SetCurrentMediaType(mf_reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                         NULL, media_type);

    /* Read back actual format to get dimensions */
    IMFMediaType* actual = NULL;
    IMFSourceReader_GetCurrentMediaType(mf_reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
    if (actual) {
        UINT64 frame_size = 0;
        IMFMediaType_GetUINT64(actual, &MF_MT_FRAME_SIZE, &frame_size);
        mf_width = (uint32_t)(frame_size >> 32);
        mf_height = (uint32_t)(frame_size & 0xFFFFFFFF);
        IMFMediaType_Release(actual);
    }
    if (media_type) IMFMediaType_Release(media_type);

    printf("[GBCAM] Webcam opened: %ux%u (device %d)\n", mf_width, mf_height, dev_index);
    mf_state = 2;
    return true;
}

bool gbcam_grab_grayscale(uint8_t* grayscale_out) {
    if (!mf_reader) return false;

    DWORD flags = 0;
    IMFSample* sample = NULL;
    HRESULT hr = IMFSourceReader_ReadSample(mf_reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                             0, NULL, &flags, NULL, &sample);
    if (FAILED(hr) || !sample) return false;

    IMFMediaBuffer* mbuf = NULL;
    IMFSample_ConvertToContiguousBuffer(sample, &mbuf);
    if (!mbuf) { IMFSample_Release(sample); return false; }

    BYTE* data = NULL;
    DWORD data_len = 0;
    IMFMediaBuffer_Lock(mbuf, &data, NULL, &data_len);

    /* Extract Y from YUY2 and downsample */
    for (int gy = 0; gy < GBCAM_HEIGHT; gy++) {
        int sy = (int)((uint64_t)gy * mf_height / GBCAM_HEIGHT);
        for (int gx = 0; gx < GBCAM_WIDTH; gx++) {
            int sx = (int)((uint64_t)gx * mf_width / GBCAM_WIDTH);
            uint32_t offset = (uint32_t)(sy * mf_width + sx) * 2;
            grayscale_out[gy * GBCAM_WIDTH + gx] =
                (offset < data_len) ? data[offset] : 0;
        }
    }

    IMFMediaBuffer_Unlock(mbuf);
    IMFMediaBuffer_Release(mbuf);
    IMFSample_Release(sample);
    return true;
}

/* ============================================================================
 * No-op fallback for unsupported platforms
 * ========================================================================== */
#else

void gbcam_list_devices(void) {
    printf("[GBCAM] No webcam support on this platform\n");
}
bool gbcam_open(const char* device) { (void)device; return false; }
void gbcam_close(void) {}
bool gbcam_grab_grayscale(uint8_t* grayscale_out) { (void)grayscale_out; return false; }

#endif /* platform backends */


/* ============================================================================
 * Common image processing pipeline (all platforms)
 * ========================================================================== */

/* Cached tile data so captures are instant */
static uint8_t tile_cache[GBCAM_TILE_COUNT * 16];
static bool cache_valid = false;
static uint32_t capture_count = 0;

bool gbcam_capture_to_sram(uint8_t* sram, uint32_t sram_size, const uint8_t cam_regs[6]) {
    capture_count++;

    /* Skip the very first capture (boot-time hardware check) */
    if (capture_count <= 1) return false;

    if (!sram || sram_size < GBCAM_IMG_OFFSET + GBCAM_IMG_SIZE) return false;

    /* Lazy-open on second capture */
    if (capture_count == 2) {
        if (!gbcam_open(NULL)) return false;
    }

    /* Try to grab latest frame (non-blocking) */
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
        int16_t* diffbuf = (int16_t*)malloc(total * sizeof(int16_t));
        if (!diffbuf) return false;

        for (int i = 0; i < total; i++) {
            int val = sharp[i];
            if (val <= lum_min) val = 0;
            else if (val >= lum_max) val = 255;
            else val = (val - lum_min) * 255 / range;
            /* Gamma ~1.8 for better midtone separation */
            int sq = (int)(val * val) / 255;
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
        free(diffbuf);

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

    /* Copy cached tiles to SRAM */
    if (cache_valid) {
        memcpy(sram + GBCAM_IMG_OFFSET, tile_cache, GBCAM_IMG_SIZE);
        return true;
    }
    return false;
}
