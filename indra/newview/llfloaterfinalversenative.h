// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
#ifndef LL_FLOATER_FINALVERSE_NATIVE_H
#define LL_FLOATER_FINALVERSE_NATIVE_H
#include "llfloater.h"
#include <memory>
class LLFloaterFinalverseNative : public LLFloater
{
public:
 explicit LLFloaterFinalverseNative(const LLSD&);
 ~LLFloaterFinalverseNative() override;
 bool postBuild() override;
 void onOpen(const LLSD&) override;
 void onClose(bool) override;
 void draw() override;
 bool handleScrollWheel(S32,S32,S32) override;
private:
 struct Impl;
 std::unique_ptr<Impl> mNative;
 void connect();
 void disconnect();
};
void llfinalverseNativeMaybeOpen();
#endif
