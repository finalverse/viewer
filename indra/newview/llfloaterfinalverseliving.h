// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// Ω-XVII.3 Living World console: the maintained C++ Viewer subscribed to the
// authoritative MutSea replication stream (libmutsea_replica), composing live
// residents with the registered Montréal scene rendered by Storm/Metal.
#ifndef LL_FLOATER_FINALVERSE_LIVING_H
#define LL_FLOATER_FINALVERSE_LIVING_H
#include "llfloater.h"
#include <memory>
class LLFloaterFinalverseLiving : public LLFloater
{
public:
    explicit LLFloaterFinalverseLiving(const LLSD&);
    ~LLFloaterFinalverseLiving() override;
    bool postBuild() override;
    void onOpen(const LLSD&) override;
    void onClose(bool) override;
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    void arrange();
    void ask(const std::string& text);
};
void llfinalverseLivingMaybeOpen();
#endif
