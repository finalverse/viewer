// SPDX-License-Identifier: LGPL-2.1-only
#include "bridge.h"
#include <iostream>
#include <functional>
using namespace living;
void refused(const std::function<void()>& f)
{
    bool caught = false;
    try
    {
        f();
    }
    catch (const std::exception&)
    {
        caught = true;
    }
    require(caught, "expected refusal");
}
std::string replace(std::string s, const std::string& a, const std::string& b)
{
    auto i = s.find(a);
    require(i != std::string::npos, "test replacement");
    s.replace(i, a.size(), b);
    return s;
}
int main()
{
    const std::string world = "8699ab0c-8242-458e-b3db-4e39249b3c56", region = "b96ee246-06f6-496f-ad70-4ae73fb711dd", definition(64, 'a');
    const std::string raw =
        R"({"schema":"finalverse.scene-update/0.1","world":"8699ab0c-8242-458e-b3db-4e39249b3c56","region":"b96ee246-06f6-496f-ad70-4ae73fb711dd","epoch":"0123456789abcdef0123456789abcdef","cursor":1,"revision":10,"previous_revision":null,"full_snapshot":true,"definition_sha256":")" +
        definition +
        R"(","residents":[{"resident":"904d952a-aca7-51da-aa01-20aab7dd9893","entity":"17777f2f-c702-8e7d-9ef7-7c4aef94c158","position":[1,2,3],"source_position_region_m":[0,0,0],"motion":"stationary"}],"assets":[],"event_refs":[]})";
    Gate g;
    g.world      = world;
    g.region     = region;
    g.definition = definition;
    auto first   = update(raw);
    require(g.admit(first, raw), "initial snapshot");
    require(!g.admit(first, raw), "exact duplicate suppression");
    auto conflicting = replace(raw, "[1,2,3]", "[4,2,3]");
    refused([&] { g.admit(update(conflicting), conflicting); });
    auto ordering = replace(raw, "\"cursor\":1", "\"cursor\":0");
    refused([&] { g.admit(update(ordering), ordering); });
    auto advance =
        replace(replace(replace(raw, "\"cursor\":1", "\"cursor\":2"), "\"revision\":10", "\"revision\":11"), "[1,2,3]", "[4,2,3]");
    require(g.admit(update(advance), advance), "committed update");
    auto regression = replace(advance, "\"revision\":11", "\"revision\":9");
    refused([&] { g.admit(update(regression), regression); });
    auto wrongworld = replace(raw, world, "7a1e0c55-6b0f-4d39-9a44-3f2a8e1c9b01");
    refused([&] { g.admit(update(wrongworld), wrongworld); });
    auto wrongregion = replace(raw, region, "5d0c7a9e-2f41-4b8e-9c3a-6e1f0b2d4a77");
    refused([&] { g.admit(update(wrongregion), wrongregion); });
    auto wrongdefinition = replace(raw, definition, std::string(64, 'b'));
    refused([&] { g.admit(update(wrongdefinition), wrongdefinition); });
    auto partial = replace(raw, "\"full_snapshot\":true", "\"full_snapshot\":false");
    refused([&] { g.admit(update(partial), partial); });
    auto unknown = replace(raw, "\"assets\":[]", "\"assets\":[],\"authority\":true");
    refused([&] { update(unknown); });
    auto duplicate = replace(raw, "\"cursor\":1", "\"cursor\":1,\"cursor\":1");
    refused([&] { update(duplicate); });
    auto invalidbody = replace(raw, "\"motion\":\"stationary\"", "\"motion\":\"stationary\",\"body_sha256\":\"bad\"");
    refused([&] { update(invalidbody); });
    auto fake = replace(raw, "\"stationary\"", "\"predicted\"");
    refused([&] { update(fake); });
    refused([&] { parse(std::string(65537, 'x')); });
    std::string depth = "0";
    for (int i = 0; i < 30; i++)
        depth = "[" + depth + "]";
    refused([&] { parse("{\"deep\":" + depth + "}"); });
    auto restart =
        replace(replace(advance, "0123456789abcdef0123456789abcdef", "11111111111111111111111111111111"), "\"cursor\":2", "\"cursor\":1");
    require(g.admit(update(restart), restart), "new epoch full snapshot");
    const std::string control =
        R"({"schema":"finalverse.presentation-control/0.1","yaw":0.4,"pitch":-0.5,"distance":500,"orbit":true,"pan":[2,0,1],"lighting":"sunset","target_resident":"904d952a-aca7-51da-aa01-20aab7dd9893"})";
    auto c = camera(control);
    require(c.orbit && c.pan[0] == 2 && c.lighting == "sunset", "typed presentation");
    for (auto pair : { std::pair{ "\"distance\":500", "\"distance\":9999" }, std::pair{ "\"sunset\"", "\"arbitrary GPU code\"" },
                       std::pair{ "[2,0,1]", "[99999,0,1]" } })
    {
        auto bad = replace(control, pair.first, pair.second);
        refused([&] { camera(bad); });
    }
    std::cout << "bridge contracts: ordering, recovery, scope, malformed input and presentation limits passed\n";
}
