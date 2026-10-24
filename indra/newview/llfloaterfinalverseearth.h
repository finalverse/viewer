// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
#ifndef LL_FLOATER_FINALVERSE_EARTH_H
#define LL_FLOATER_FINALVERSE_EARTH_H
#include "llfloater.h"
class LLFloaterFinalverseEarth : public LLFloater
{
public:
    explicit LLFloaterFinalverseEarth(const LLSD& key):LLFloater(key){}
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
private:
    void run(const std::string& action);
    void render(const LLSD& result);
    void buttons();
    bool mBusy=false;
    LLUUID mOwner,mRegion;
    std::string mPlan,mStatus;
    LLSD mDestination;
};

// Separate from the existing Earth creation/approval workflow.
class LLFloaterFinalverseLivingEarth : public LLFloater
{
public:
    explicit LLFloaterFinalverseLivingEarth(const LLSD& key):LLFloater(key){}
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
private:
    void verify(bool arrival);
    void display(const std::string& message);
    LLSD mHandoff;
    bool mBusy=false;
};
// Read-only rendering placement, valid only for the current authenticated native session.
LLSD llfinalverseLivingPlacement();

#endif
