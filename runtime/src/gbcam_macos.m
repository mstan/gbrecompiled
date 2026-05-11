/**
 * @file gbcam_macos.m
 * @brief Game Boy Camera webcam capture — macOS AVFoundation backend
 */
#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#include "gbcam.h"
#include <string.h>

/* ---- Frame grabber delegate ---- */
@interface GBCamGrabber : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic) uint8_t* grayscale;
@property (nonatomic) BOOL frameReady;
@property (nonatomic) int srcWidth;
@property (nonatomic) int srcHeight;
@end

@implementation GBCamGrabber
- (void)captureOutput:(AVCaptureOutput*)output
 didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        fromConnection:(AVCaptureConnection*)connection {
    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer || !self.grayscale) return;

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);
    uint8_t* baseAddress = (uint8_t*)CVPixelBufferGetBaseAddress(imageBuffer);

    self.srcWidth = (int)width;
    self.srcHeight = (int)height;

    /* Downsample to 128x112 grayscale from BGRA */
    for (int gy = 0; gy < GBCAM_HEIGHT; gy++) {
        int sy = (int)((uint64_t)gy * height / GBCAM_HEIGHT);
        for (int gx = 0; gx < GBCAM_WIDTH; gx++) {
            int sx = (int)((uint64_t)gx * width / GBCAM_WIDTH);
            uint8_t* pixel = baseAddress + sy * bytesPerRow + sx * 4;
            /* BGRA -> luminance: 0.114*B + 0.587*G + 0.299*R */
            int lum = (pixel[0] * 29 + pixel[1] * 150 + pixel[2] * 77) / 256;
            self.grayscale[gy * GBCAM_WIDTH + gx] = (uint8_t)lum;
        }
    }

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    self.frameReady = YES;
}
@end

/* ---- Static state ---- */
static AVCaptureSession* session = nil;
static GBCamGrabber* grabber = nil;
static uint8_t frame_buffer[GBCAM_WIDTH * GBCAM_HEIGHT];
static int cam_state = 0; /* 0=not tried, 1=failed, 2=open */

int gbcam_enumerate_devices(GBCamDevice* out, int max_count) {
    NSArray* devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
    int count = (int)devices.count;
    if (!out) return count;
    int written = 0;
    for (int i = 0; i < count && written < max_count; i++) {
        AVCaptureDevice* dev = devices[i];
        snprintf(out[written].path, sizeof(out[written].path), "%d", i);
        snprintf(out[written].label, sizeof(out[written].label), "%s",
                 dev.localizedName.UTF8String);
        written++;
    }
    return count;
}

void gbcam_list_devices(void) {
    GBCamDevice devs[16];
    int n = gbcam_enumerate_devices(devs, 16);
    printf("[GBCAM] Available video devices:\n");
    for (int i = 0; i < n; i++) {
        printf("[GBCAM]   %s - %s\n", devs[i].path, devs[i].label);
    }
    printf("[GBCAM] Set GBCAM_DEVICE=N to choose a camera (default: 0)\n");
}

void gbcam_close(void) {
    if (session) {
        [session stopRunning];
        session = nil;
    }
    grabber = nil;
    /* 0 = "not attempted yet" so gbcam_open will retry; 1 stays reserved
     * for permanent open failures inside gbcam_open. */
    cam_state = 0;
}

bool gbcam_open(const char* device) {
    if (cam_state == 1) return false;
    if (cam_state == 2) return true;

    if (!device) device = getenv("GBCAM_DEVICE");
    int dev_index = device ? atoi(device) : 0;

    gbcam_list_devices();

    @autoreleasepool {
        NSArray* devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
        if ((NSUInteger)dev_index >= devices.count) {
            printf("[GBCAM] Device index %d not found\n", dev_index);
            cam_state = 1;
            return false;
        }

        AVCaptureDevice* dev = devices[dev_index];
        printf("[GBCAM] Using %s\n", dev.localizedName.UTF8String);
        char idx_str[16];
        snprintf(idx_str, sizeof(idx_str), "%d", dev_index);
        extern void gbcam_internal_set_current(const char*);
        gbcam_internal_set_current(idx_str);

        NSError* error = nil;
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&error];
        if (!input) {
            printf("[GBCAM] Failed to create device input: %s\n",
                   error.localizedDescription.UTF8String);
            cam_state = 1;
            return false;
        }

        session = [[AVCaptureSession alloc] init];
        session.sessionPreset = AVCaptureSessionPresetLow;
        [session addInput:input];

        AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
        output.videoSettings = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)
        };
        output.alwaysDiscardsLateVideoFrames = YES;

        grabber = [[GBCamGrabber alloc] init];
        grabber.grayscale = frame_buffer;
        grabber.frameReady = NO;

        dispatch_queue_t queue = dispatch_queue_create("gbcam", DISPATCH_QUEUE_SERIAL);
        [output setSampleBufferDelegate:grabber queue:queue];
        [session addOutput:output];

        [session startRunning];
    }

    printf("[GBCAM] Webcam streaming (non-blocking)\n");
    cam_state = 2;
    return true;
}

bool gbcam_grab_grayscale(uint8_t* grayscale_out) {
    if (cam_state != 2 || !grabber) return false;
    if (!grabber.frameReady) return false;

    memcpy(grayscale_out, frame_buffer, GBCAM_WIDTH * GBCAM_HEIGHT);
    grabber.frameReady = NO;
    return true;
}

#endif /* __APPLE__ */
