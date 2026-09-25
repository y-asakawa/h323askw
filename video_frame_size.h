// Copyright (c) 2026 Yoshifumi Asakawa
// SPDX-License-Identifier: MPL-1.0

#ifndef H323ASKW_VIDEO_FRAME_SIZE_H
#define H323ASKW_VIDEO_FRAME_SIZE_H

#include <cstddef>

namespace VideoFrameSize {

constexpr unsigned kMaxDimension = 4096;

inline bool YUV420P(unsigned width, unsigned height, size_t& ySize, size_t& frameSize)
{
    if (width < 2 || height < 2 || width > kMaxDimension || height > kMaxDimension ||
        (width & 1u) != 0 || (height & 1u) != 0) {
        return false;
    }

    ySize = static_cast<size_t>(width) * height;
    frameSize = ySize + ySize / 2;
    return true;
}

inline bool RGB24(unsigned width, unsigned height, size_t& frameSize)
{
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        return false;
    }

    frameSize = static_cast<size_t>(width) * height * 3;
    return true;
}

}  // namespace VideoFrameSize

#endif  // H323ASKW_VIDEO_FRAME_SIZE_H
