// Copyright (c) 2026 Yoshifumi Asakawa
// SPDX-License-Identifier: MPL-1.0

#ifndef H323ASKW_VIDEO_LETTERBOX_H
#define H323ASKW_VIDEO_LETTERBOX_H

#include "video_frame_size.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace VideoLetterbox {

inline bool IsCifRaster(unsigned width, unsigned height)
{
    return (width == 176 && height == 144) ||
           (width == 352 && height == 288) ||
           (width == 704 && height == 576) ||
           (width == 1408 && height == 1152);
}

inline bool FitTile(unsigned sourceWidth, unsigned sourceHeight,
                    unsigned tileWidth, unsigned tileHeight, bool cifRaster,
                    unsigned & copyWidth, unsigned & copyHeight)
{
    if (sourceWidth == 0 || sourceHeight == 0 || tileWidth == 0 || tileHeight == 0)
        return false;

    const uint64_t sarWidth = cifRaster ? 12 : 1;
    const uint64_t sarHeight = cifRaster ? 11 : 1;
    copyWidth = tileWidth;
    copyHeight = tileHeight;
    if (static_cast<uint64_t>(sourceWidth) * tileHeight * sarHeight >
        static_cast<uint64_t>(sourceHeight) * tileWidth * sarWidth) {
        copyHeight = static_cast<unsigned>(
            static_cast<uint64_t>(tileWidth) * sarWidth * sourceHeight /
            (static_cast<uint64_t>(sourceWidth) * sarHeight));
    } else {
        copyWidth = static_cast<unsigned>(
            static_cast<uint64_t>(tileHeight) * sarHeight * sourceWidth /
            (static_cast<uint64_t>(sourceHeight) * sarWidth));
    }
    return copyWidth != 0 && copyHeight != 0;
}

// CIF pixels have a 12:11 sample aspect ratio. Put a 16:9 source in a
// 352:288 (or QCIF) raster without stretching it on a 4:3 display.
inline bool WideSourceToCif(uint8_t * frame, size_t bytes, unsigned width,
                            unsigned height, std::vector<uint8_t> & scratch)
{
    size_t ySize = 0;
    size_t frameSize = 0;
    if (frame == nullptr || !VideoFrameSize::YUV420P(width, height, ySize, frameSize) ||
        bytes < frameSize || static_cast<size_t>(width) * 9 != static_cast<size_t>(height) * 11)
        return false;

    const unsigned activeHeight = height * 3 / 4;
    const unsigned top = (height - activeHeight) / 2;
    if (activeHeight == 0 || (activeHeight & 1u) != 0 || (top & 1u) != 0)
        return false;

    scratch.assign(frame, frame + frameSize);
    const uint8_t * source = scratch.data();
    std::memset(frame, 16, ySize);
    std::memset(frame + ySize, 128, frameSize - ySize);

    for (unsigned plane = 0; plane < 3; ++plane) {
        const unsigned planeWidth = plane == 0 ? width : width / 2;
        const unsigned sourceHeight = plane == 0 ? height : height / 2;
        const unsigned outputHeight = plane == 0 ? activeHeight : activeHeight / 2;
        const unsigned topRows = plane == 0 ? top : top / 2;
        const size_t offset = plane == 0 ? 0 : ySize + (plane - 1) * (ySize / 4);
        for (unsigned row = 0; row < outputHeight; ++row) {
            const unsigned sourceRow = row * sourceHeight / outputHeight;
            std::memcpy(frame + offset + static_cast<size_t>(topRows + row) * planeWidth,
                        source + offset + static_cast<size_t>(sourceRow) * planeWidth, planeWidth);
        }
    }
    return true;
}

}  // namespace VideoLetterbox

#endif  // H323ASKW_VIDEO_LETTERBOX_H
