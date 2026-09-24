// Copyright (c) 2026 Yoshifumi Asakawa
// SPDX-License-Identifier: MPL-1.0

#include "vision_person_mask.h"

#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

static inline uint8_t ClampByte(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

static inline unsigned InferenceMaxSideForStrength(int strength)
{
    switch (strength) {
        case 0: return 256;
        case 1: return 320;
        default: return 384;
    }
}

static inline unsigned RefreshIntervalForStrength(int strength)
{
    switch (strength) {
        case 0: return 3;
        case 1: return 2;
        default: return 1;
    }
}

} // namespace

struct VisionPersonMaskGenerator::Impl
{
    __strong VNGeneratePersonSegmentationRequest* request = nil;
    bool available = false;
    uint64_t frameCounter = 0;
    unsigned cachedWidth = 0;
    unsigned cachedHeight = 0;
    std::vector<uint8_t> cachedMask;
    std::vector<uint8_t> smoothBuffer;
};

VisionPersonMaskGenerator::VisionPersonMaskGenerator()
    : m_impl(new Impl())
{
    @autoreleasepool {
        if (@available(macOS 11.0, *)) {
            m_impl->request = [[VNGeneratePersonSegmentationRequest alloc] init];
            if (m_impl->request != nil) {
                m_impl->request.qualityLevel = VNGeneratePersonSegmentationRequestQualityLevelBalanced;
                m_impl->request.outputPixelFormat = kCVPixelFormatType_OneComponent8;
                m_impl->request.preferBackgroundProcessing = YES;
                m_impl->available = true;
            }
        } else {
            m_impl->available = false;
        }
    }
}

VisionPersonMaskGenerator::~VisionPersonMaskGenerator() = default;

bool VisionPersonMaskGenerator::IsAvailable() const
{
    return m_impl && m_impl->available;
}

bool VisionPersonMaskGenerator::GenerateBackgroundMask(const uint8_t* yPlane,
                                                       const uint8_t* uPlane,
                                                       const uint8_t* vPlane,
                                                       unsigned width,
                                                       unsigned height,
                                                       int strength,
                                                       uint8_t* outMaskY,
                                                       size_t outMaskSize)
{
    if (!m_impl || !m_impl->available || !yPlane || !uPlane || !vPlane || !outMaskY) {
        return false;
    }

    if (width == 0 || height == 0 || outMaskSize < static_cast<size_t>(width) * height) {
        return false;
    }

    if (strength < 0) {
        strength = 0;
    } else if (strength > 2) {
        strength = 2;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    const unsigned refreshInterval = RefreshIntervalForStrength(strength);
    if (!m_impl->cachedMask.empty() &&
        m_impl->cachedWidth == width &&
        m_impl->cachedHeight == height &&
        (m_impl->frameCounter % refreshInterval) != 0) {
        std::memcpy(outMaskY, m_impl->cachedMask.data(), pixelCount);
        ++m_impl->frameCounter;
        return true;
    }

    ++m_impl->frameCounter;

    const unsigned maxSide = InferenceMaxSideForStrength(strength);
    unsigned inferWidth = width;
    unsigned inferHeight = height;
    if (inferWidth >= inferHeight && inferWidth > maxSide) {
        inferHeight = std::max(1u, static_cast<unsigned>((static_cast<uint64_t>(inferHeight) * maxSide) / inferWidth));
        inferWidth = maxSide;
    } else if (inferHeight > inferWidth && inferHeight > maxSide) {
        inferWidth = std::max(1u, static_cast<unsigned>((static_cast<uint64_t>(inferWidth) * maxSide) / inferHeight));
        inferHeight = maxSide;
    }

    CVPixelBufferRef inputBuffer = nullptr;
    NSDictionary* attrs = @{
        (id)kCVPixelBufferCGImageCompatibilityKey: @NO,
        (id)kCVPixelBufferCGBitmapContextCompatibilityKey: @NO
    };

    CVReturn createResult = CVPixelBufferCreate(kCFAllocatorDefault,
                                                 inferWidth,
                                                 inferHeight,
                                                 kCVPixelFormatType_32BGRA,
                                                 (__bridge CFDictionaryRef)attrs,
                                                 &inputBuffer);
    if (createResult != kCVReturnSuccess || inputBuffer == nullptr) {
        if (!m_impl->cachedMask.empty() &&
            m_impl->cachedWidth == width &&
            m_impl->cachedHeight == height) {
            std::memcpy(outMaskY, m_impl->cachedMask.data(), pixelCount);
            return true;
        }
        return false;
    }

    CVPixelBufferLockBaseAddress(inputBuffer, 0);
    uint8_t* dstBase = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(inputBuffer));
    const size_t dstStride = static_cast<size_t>(CVPixelBufferGetBytesPerRow(inputBuffer));
    const unsigned uvWidth = width / 2;

    for (unsigned y = 0; y < inferHeight; ++y) {
        uint8_t* row = dstBase + static_cast<size_t>(y) * dstStride;
        const unsigned srcY = std::min(height - 1, static_cast<unsigned>((static_cast<uint64_t>(y) * height) / inferHeight));
        const unsigned srcUvY = srcY / 2;

        for (unsigned x = 0; x < inferWidth; ++x) {
            const unsigned srcX = std::min(width - 1, static_cast<unsigned>((static_cast<uint64_t>(x) * width) / inferWidth));
            const unsigned srcUvX = srcX / 2;

            const int yy = static_cast<int>(yPlane[static_cast<size_t>(srcY) * width + srcX]);
            const int uu = static_cast<int>(uPlane[static_cast<size_t>(srcUvY) * uvWidth + srcUvX]);
            const int vv = static_cast<int>(vPlane[static_cast<size_t>(srcUvY) * uvWidth + srcUvX]);

            const int c = std::max(0, yy - 16);
            const int d = uu - 128;
            const int e = vv - 128;

            const int r = (298 * c + 409 * e + 128) >> 8;
            const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            const int b = (298 * c + 516 * d + 128) >> 8;

            uint8_t* px = row + static_cast<size_t>(x) * 4;
            px[0] = ClampByte(b);
            px[1] = ClampByte(g);
            px[2] = ClampByte(r);
            px[3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(inputBuffer, 0);

    bool success = false;
    @autoreleasepool {
        NSError* error = nil;
        VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:inputBuffer options:@{}];
        BOOL performed = [handler performRequests:@[m_impl->request] error:&error];
        if (performed) {
            VNPixelBufferObservation* observation = nil;
            if (m_impl->request.results.count > 0) {
                observation = (VNPixelBufferObservation*)m_impl->request.results.firstObject;
            }

            if (observation != nil && observation.pixelBuffer != nullptr) {
                CVPixelBufferRef maskBuffer = observation.pixelBuffer;
                CVPixelBufferLockBaseAddress(maskBuffer, kCVPixelBufferLock_ReadOnly);

                const uint8_t* maskBase = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(maskBuffer));
                const size_t maskStride = static_cast<size_t>(CVPixelBufferGetBytesPerRow(maskBuffer));
                const unsigned maskW = static_cast<unsigned>(CVPixelBufferGetWidth(maskBuffer));
                const unsigned maskH = static_cast<unsigned>(CVPixelBufferGetHeight(maskBuffer));

                for (unsigned y = 0; y < height; ++y) {
                    const unsigned my = std::min(maskH - 1, static_cast<unsigned>((static_cast<uint64_t>(y) * maskH) / height));
                    const uint8_t* srcRow = maskBase + static_cast<size_t>(my) * maskStride;
                    uint8_t* dstRow = outMaskY + static_cast<size_t>(y) * width;
                    for (unsigned x = 0; x < width; ++x) {
                        const unsigned mx = std::min(maskW - 1, static_cast<unsigned>((static_cast<uint64_t>(x) * maskW) / width));
                        const uint8_t fg = srcRow[mx];
                        dstRow[x] = static_cast<uint8_t>(255 - fg);
                    }
                }

                CVPixelBufferUnlockBaseAddress(maskBuffer, kCVPixelBufferLock_ReadOnly);
                success = true;
            }
        } else {
            (void)error;
        }
    }

    CVPixelBufferRelease(inputBuffer);

    if (!success) {
        if (!m_impl->cachedMask.empty() &&
            m_impl->cachedWidth == width &&
            m_impl->cachedHeight == height) {
            std::memcpy(outMaskY, m_impl->cachedMask.data(), pixelCount);
            return true;
        }
        return false;
    }

    if (m_impl->smoothBuffer.size() != pixelCount) {
        m_impl->smoothBuffer.assign(pixelCount, 0);
    }

    for (unsigned y = 0; y < height; ++y) {
        const unsigned y0 = (y == 0) ? 0 : (y - 1);
        const unsigned y1 = std::min(height - 1, y + 1);
        for (unsigned x = 0; x < width; ++x) {
            const unsigned x0 = (x == 0) ? 0 : (x - 1);
            const unsigned x1 = std::min(width - 1, x + 1);

            unsigned sum = 0;
            unsigned count = 0;
            for (unsigned yy = y0; yy <= y1; ++yy) {
                const size_t rowOffset = static_cast<size_t>(yy) * width;
                for (unsigned xx = x0; xx <= x1; ++xx) {
                    sum += outMaskY[rowOffset + xx];
                    ++count;
                }
            }

            m_impl->smoothBuffer[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(sum / count);
        }
    }

    std::memcpy(outMaskY, m_impl->smoothBuffer.data(), pixelCount);
    m_impl->cachedMask.assign(outMaskY, outMaskY + pixelCount);
    m_impl->cachedWidth = width;
    m_impl->cachedHeight = height;
    return true;
}

#else

struct VisionPersonMaskGenerator::Impl {};

VisionPersonMaskGenerator::VisionPersonMaskGenerator()
    : m_impl(new Impl())
{
}

VisionPersonMaskGenerator::~VisionPersonMaskGenerator() = default;

bool VisionPersonMaskGenerator::IsAvailable() const
{
    return false;
}

bool VisionPersonMaskGenerator::GenerateBackgroundMask(const uint8_t*,
                                                       const uint8_t*,
                                                       const uint8_t*,
                                                       unsigned,
                                                       unsigned,
                                                       int,
                                                       uint8_t*,
                                                       size_t)
{
    return false;
}

#endif // defined(__APPLE__)
