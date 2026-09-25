// Copyright (c) 2026 Yoshifumi Asakawa
// SPDX-License-Identifier: MPL-1.0

#include "../video_frame_size.h"

#include <cassert>
#include <limits>

int main()
{
    size_t ySize = 0;
    size_t frameSize = 0;

    assert(VideoFrameSize::YUV420P(1280, 720, ySize, frameSize));
    assert(ySize == 1280u * 720u);
    assert(frameSize == 1280u * 720u * 3u / 2u);
    assert(VideoFrameSize::YUV420P(4096, 4096, ySize, frameSize));
    assert(frameSize == 4096u * 4096u * 3u / 2u);
    assert(!VideoFrameSize::YUV420P(0, 720, ySize, frameSize));
    assert(!VideoFrameSize::YUV420P(1279, 720, ySize, frameSize));
    assert(!VideoFrameSize::YUV420P(4098, 720, ySize, frameSize));
    assert(!VideoFrameSize::YUV420P(65536, 65536, ySize, frameSize));
    assert(!VideoFrameSize::YUV420P(std::numeric_limits<unsigned>::max(),
                                    std::numeric_limits<unsigned>::max(), ySize, frameSize));

    assert(VideoFrameSize::RGB24(1, 1, frameSize) && frameSize == 3);
    assert(VideoFrameSize::RGB24(1920, 1080, frameSize) && frameSize == 1920u * 1080u * 3u);
    assert(!VideoFrameSize::RGB24(0, 1080, frameSize));
    assert(!VideoFrameSize::RGB24(65536, 65536, frameSize));
}
