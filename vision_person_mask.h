// Copyright (c) 2026 Yoshifumi Asakawa
// SPDX-License-Identifier: MPL-1.0

#ifndef H323ASKW_VISION_PERSON_MASK_H
#define H323ASKW_VISION_PERSON_MASK_H

#include <cstddef>
#include <cstdint>
#include <memory>

class VisionPersonMaskGenerator
{
public:
    VisionPersonMaskGenerator();
    ~VisionPersonMaskGenerator();

    bool IsAvailable() const;

    bool GenerateBackgroundMask(const uint8_t* yPlane,
                                const uint8_t* uPlane,
                                const uint8_t* vPlane,
                                unsigned width,
                                unsigned height,
                                int strength,
                                uint8_t* outMaskY,
                                size_t outMaskSize);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // H323ASKW_VISION_PERSON_MASK_H
