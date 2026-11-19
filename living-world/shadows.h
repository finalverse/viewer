// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <memory>
namespace living
{
// Optional source-pinned OGRE compositor extension. Public APIs only; never
// reads or writes a world. Caller lifetime ends before storm::Renderer.
class Shadows
{
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    explicit Shadows(bool enable);
    ~Shadows();
    void after_first_frame();
    bool active() const;
};
} // namespace living
