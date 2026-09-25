// Copyright (c) 2026 Yoshifumi Asakawa
// SPDX-License-Identifier: MPL-1.0

#include "../video_letterbox.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

static void CheckCif(unsigned width, unsigned height)
{
    size_t ySize = 0;
    size_t frameSize = 0;
    assert(VideoFrameSize::YUV420P(width, height, ySize, frameSize));
    std::vector<uint8_t> frame(frameSize + 16, 0xee);
    std::fill(frame.begin(), frame.begin() + ySize, 100);
    std::fill(frame.begin() + ySize, frame.begin() + ySize + ySize / 4, 80);
    std::fill(frame.begin() + ySize + ySize / 4, frame.begin() + frameSize, 160);
    std::vector<uint8_t> scratch;

    assert(VideoLetterbox::WideSourceToCif(frame.data(), frameSize, width, height, scratch));
    const unsigned top = height / 8;
    const unsigned bottom = top + height * 3 / 4;
    for (unsigned plane = 0; plane < 3; ++plane) {
        const unsigned planeWidth = plane == 0 ? width : width / 2;
        const unsigned planeHeight = plane == 0 ? height : height / 2;
        const unsigned topRow = plane == 0 ? top : top / 2;
        const unsigned bottomRow = plane == 0 ? bottom : bottom / 2;
        const size_t offset = plane == 0 ? 0 : ySize + (plane - 1) * ySize / 4;
        const uint8_t barValue = plane == 0 ? 16 : 128;
        const uint8_t imageValue = plane == 0 ? 100 : (plane == 1 ? 80 : 160);
        for (unsigned row = 0; row < planeHeight; ++row) {
            for (unsigned col = 0; col < planeWidth; ++col) {
                const uint8_t expected = row < topRow || row >= bottomRow ? barValue : imageValue;
                assert(frame[offset + static_cast<size_t>(row) * planeWidth + col] == expected);
            }
        }
    }
    for (size_t i = frameSize; i < frame.size(); ++i)
        assert(frame[i] == 0xee);
}

int main()
{
    CheckCif(352, 288);
    CheckCif(176, 144);

    unsigned width = 0;
    unsigned height = 0;
    assert(VideoLetterbox::IsCifRaster(352, 288));
    assert(VideoLetterbox::IsCifRaster(176, 144));
    assert(!VideoLetterbox::IsCifRaster(1280, 720));
    assert(VideoLetterbox::FitTile(1280, 720, 352, 288, true, width, height));
    assert(width == 352 && height == 216);
    assert(VideoLetterbox::FitTile(1280, 720, 352, 288, false, width, height));
    assert(width == 352 && height == 198);
    assert(VideoLetterbox::FitTile(640, 480, 352, 288, true, width, height));
    assert(width == 352 && height == 288);
    assert(VideoLetterbox::FitTile(1280, 720, 176, 288, true, width, height));
    assert(width == 176 && height == 108);

    std::vector<uint8_t> frame(352 * 288 * 3 / 2, 0x55);
    const std::vector<uint8_t> original = frame;
    std::vector<uint8_t> scratch;
    assert(!VideoLetterbox::WideSourceToCif(frame.data(), frame.size() - 1,
                                            352, 288, scratch));
    assert(!VideoLetterbox::WideSourceToCif(frame.data(), frame.size(),
                                            1280, 720, scratch));
    assert(frame == original);
}
