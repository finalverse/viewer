// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// FINALVERSE shell (Ω-XVIII Viewer Renaissance): a full-window, AI-native spatial
// browser over the universal spatial contracts (FVP domain 4 / SpatialDB), Storm scene
// contexts and the live MutSea world. Replaces the legacy client chrome while open.
#pragma once
#include "llfloater.h"
#include <memory>

class LLFloaterFinalverseShell : public LLFloater
{
public:
    explicit LLFloaterFinalverseShell(const LLSD& key);
    ~LLFloaterFinalverseShell() override;
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleKeyHere(KEY key, MASK mask) override;

    struct Impl;

private:
    void arrange();
    std::unique_ptr<Impl> m;
};

// Opens the shell once at startup when FINALVERSE_SHELL=1.
void llfinalverseShellMaybeOpen();
