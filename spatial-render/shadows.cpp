// SPDX-License-Identifier: LGPL-2.1-only
// Frozen capture-adapter feature, not an addition to Storm's public API.
#include "shadows.h"
#include "bridge.h"
#include "OgreRoot.h"
#include "OgreSceneManager.h"
#include "OgreMovableObject.h"
#include "Compositor/OgreCompositorManager2.h"
#include "Compositor/OgreCompositorNodeDef.h"
#include "Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h"
namespace living
{
struct Shadows::Impl final
{
    bool configured = false;
    explicit Impl(bool enabled)
    {
        if (!enabled)
            return;
        auto* root = Ogre::Root::getSingletonPtr();
        require(root != nullptr, "shadow extension requires initialized renderer");
        auto* manager = root->getCompositorManager2();
        require(manager->getNumWorkspaces() <= 1, "shadow extension requires isolated frozen renderer");
        (void)manager->getShadowNodeDefinition("PbsMaterialsShadowNode");
        const Ogre::String name = "AutoGen " + Ogre::IdString("Storm Workspace/Node").getReleaseText();
        auto*              node = manager->getNodeDefinitionNonConst(name);
        require(node->getNumTargetPasses() == 1, "frozen main compositor target");
        auto& passes = node->getTargetPass(0)->getCompositorPassesNonConst();
        require(passes.size() == 1 && passes.front()->getType() == Ogre::PASS_SCENE, "frozen main scene pass");
        auto* scene = root->getSceneManager("StormScene");
        scene->setShadowFarDistance(600.f);
        auto objects = scene->getMovableObjectIterator("ManualObject2");
        while (objects.hasMoreElements())
        {
            auto* object = objects.getNext();
            if (object->getLocalRadius() > 1000.f)
                object->setCastShadows(false);
        }
        // The captured stream gets a fresh workspace for every capture. Its definition
        // can be configured through public APIs without replacing the workspace owned
        // privately by Storm, or editing the frozen source. The existing window is
        // intentionally unchanged; this is a CPU captured-frame bridge extension.
        static_cast<Ogre::CompositorPassSceneDef*>(passes.front())->mShadowNode = Ogre::IdString("PbsMaterialsShadowNode");
        configured                                                              = true;
    }
};
Shadows::Shadows(bool enabled) : impl(std::make_unique<Impl>(enabled))
{
}
Shadows::~Shadows() = default;
void Shadows::after_first_frame()
{
}
bool Shadows::active() const
{ return impl->configured; }
} // namespace living
