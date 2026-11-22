// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <rapidjson/document.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace living
{
using Vec = std::array<double, 3>;
using J   = rapidjson::Value;
inline void require(bool b, const char* e)
{
    if (!b)
        throw std::runtime_error(e);
}
inline void bounded_tree(const J& j, unsigned depth = 0)
{
    require(depth <= 24, "JSON depth limit");
    if (j.IsObject())
    {
        std::set<std::string> keys;
        for (auto i = j.MemberBegin(); i != j.MemberEnd(); ++i)
        {
            require(keys.insert(i->name.GetString()).second, "duplicate JSON key");
            bounded_tree(i->value, depth + 1);
        }
    }
    if (j.IsArray())
        for (const auto& v : j.GetArray())
            bounded_tree(v, depth + 1);
}
inline rapidjson::Document parse(const std::string& raw)
{
    require(!raw.empty() && raw.size() <= 65536, "JSON byte budget");
    rapidjson::Document d;
    d.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(raw.data(), raw.size());
    require(!d.HasParseError() && d.IsObject(), "JSON object required");
    bounded_tree(d);
    return d;
}
inline void keys(const J& v, std::initializer_list<const char*> mandatory, std::initializer_list<const char*> optional = {})
{
    require(v.IsObject(), "object required");
    std::set<std::string> names;
    for (auto k : mandatory)
    {
        require(v.HasMember(k), "required field");
        names.insert(k);
    }
    for (auto k : optional)
        names.insert(k);
    for (auto i = v.MemberBegin(); i != v.MemberEnd(); ++i)
        require(names.contains(i->name.GetString()), "unknown field");
}
inline std::string text(const J& v, size_t max = 128)
{
    require(v.IsString() && v.GetStringLength() > 0 && v.GetStringLength() <= max, "bounded string required");
    return { v.GetString(), v.GetStringLength() };
}
inline bool hex(const std::string& s, size_t n)
{ return s.size() == n && s.find_first_not_of("0123456789abcdef") == std::string::npos; }
inline bool uuid(const std::string& s)
{
    if (s.size() != 36)
        return false;
    std::string flat;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (s[i] != '-')
                return false;
        }
        else
            flat += s[i];
    }
    return hex(flat, 32) && flat != std::string(32, '0');
}
inline uint64_t integer(const J& v)
{
    require(v.IsUint64(), "unsigned integer required");
    return v.GetUint64();
}
inline double number(const J& v, double low, double high)
{
    require(v.IsNumber() && std::isfinite(v.GetDouble()) && v.GetDouble() >= low && v.GetDouble() <= high, "number bounds");
    return v.GetDouble();
}
inline Vec point(const J& v, double bound = 1e7)
{
    require(v.IsArray() && v.Size() == 3, "position array required");
    return { number(v[0], -bound, bound), number(v[1], -bound, bound), number(v[2], -bound, bound) };
}
struct Camera
{
    double      yaw = .4, pitch = -.65, distance = 500;
    bool        orbit = false;
    Vec         pan{};
    std::string lighting = "daylight", target;
};
inline Camera camera(const std::string& raw)
{
    auto d = parse(raw);
    keys(d, { "schema", "yaw", "pitch", "distance", "orbit", "pan", "lighting" }, { "target_resident" });
    require(text(d["schema"]) == "finalverse.presentation-control/0.1", "presentation schema");
    Camera c;
    c.yaw      = number(d["yaw"], -100, 100);
    c.pitch    = number(d["pitch"], -1.5, -.05);
    c.distance = number(d["distance"], 2, 2000);
    require(d["orbit"].IsBool(), "orbit boolean");
    c.orbit    = d["orbit"].GetBool();
    c.pan      = point(d["pan"], 2000);
    c.lighting = text(d["lighting"]);
    require(c.lighting == "daylight" || c.lighting == "sunset", "lighting preset");
    if (d.HasMember("target_resident") && !d["target_resident"].IsNull())
    {
        c.target = text(d["target_resident"]);
        require(uuid(c.target), "target resident UUID");
    }
    return c;
}
struct Resident
{
    std::string resident, entity, body, motion;
    Vec         position;
};
struct Update
{
    std::string           world, region, epoch, definition;
    uint64_t              cursor = 0, revision = 0, previous = 0;
    bool                  has_previous = false, full = false;
    std::vector<Resident> residents;
};
inline Update update(const std::string& raw)
{
    auto d = parse(raw);
    keys(d, { "schema", "world", "region", "epoch", "cursor", "revision", "previous_revision", "full_snapshot", "definition_sha256",
              "residents", "assets", "event_refs" });
    require(text(d["schema"]) == "finalverse.scene-update/0.1", "scene update schema");
    Update u;
    u.world      = text(d["world"]);
    u.region     = text(d["region"]);
    u.epoch      = text(d["epoch"]);
    u.definition = text(d["definition_sha256"]);
    require(uuid(u.world) && uuid(u.region) && hex(u.epoch, 32) && hex(u.definition, 64), "scene identity/hash");
    u.cursor   = integer(d["cursor"]);
    u.revision = integer(d["revision"]);
    if (!d["previous_revision"].IsNull())
    {
        u.previous     = integer(d["previous_revision"]);
        u.has_previous = true;
    }
    require(d["full_snapshot"].IsBool(), "snapshot boolean");
    u.full = d["full_snapshot"].GetBool();
    require(d["residents"].IsArray() && d["residents"].Size() <= 16, "resident budget");
    std::set<std::string> residents, entities;
    for (const auto& r : d["residents"].GetArray())
    {
        keys(r, { "resident", "entity", "position", "source_position_region_m", "motion" }, { "body_sha256" });
        Resident a;
        a.resident = text(r["resident"]);
        a.entity   = text(r["entity"]);
        a.position = point(r["position"]);
        a.motion   = text(r["motion"]);
        (void)point(r["source_position_region_m"]);
        require(uuid(a.resident) && uuid(a.entity) && residents.insert(a.resident).second && entities.insert(a.entity).second,
                "unique resident/entity UUID");
        require(a.motion == "committed" || a.motion == "stationary", "committed motion state");
        if (r.HasMember("body_sha256"))
        {
            a.body = text(r["body_sha256"]);
            require(hex(a.body, 64), "body digest");
        }
        u.residents.push_back(a);
    }
    require(d["assets"].IsArray() && d["assets"].Size() <= 128, "asset reference budget");
    for (const auto& a : d["assets"].GetArray())
    {
        keys(a, { "entity", "asset", "sha256", "transform" });
        require(uuid(text(a["entity"])) && uuid(text(a["asset"])) && hex(text(a["sha256"]), 64), "asset identity");
        const auto& t = a["transform"];
        keys(t, { "translation", "rotation", "scale" });
        (void)point(t["translation"]);
        auto s = point(t["scale"], 1000);
        for (auto v : s)
            require(v > 0, "positive scale");
        require(t["rotation"].IsArray() && t["rotation"].Size() == 4, "quaternion");
        double norm = 0;
        for (auto& v : t["rotation"].GetArray())
        {
            auto x = number(v, -1, 1);
            norm += x * x;
        }
        require(std::abs(norm - 1) < 1e-5, "unit quaternion");
    }
    require(d["event_refs"].IsArray() && d["event_refs"].Size() <= 128, "event budget");
    for (const auto& e : d["event_refs"].GetArray())
    {
        keys(e, { "sequence", "sha256", "commands" });
        (void)integer(e["sequence"]);
        require(hex(text(e["sha256"]), 64), "event digest");
        require(e["commands"].IsArray() && e["commands"].Size() <= 32, "command refs");
        for (const auto& c : e["commands"].GetArray())
            require(uuid(text(c)), "command UUID");
    }
    return u;
}
inline bool same_poses(const Update& a, const Update& b)
{
    if (a.residents.size() != b.residents.size())
        return false;
    for (const auto& x : a.residents)
    {
        auto i = std::find_if(b.residents.begin(), b.residents.end(), [&](const auto& v) { return v.entity == x.entity; });
        if (i == b.residents.end() || i->resident != x.resident || i->position != x.position || i->body != x.body)
            return false;
    }
    return true;
}
struct Gate
{
    std::string world, region, definition, raw;
    Update      last;
    bool        initialized = false;
    bool        admit(const Update& u, const std::string& bytes)
    {
        require(u.world == world && u.region == region && u.definition == definition, "scene world/region/definition scope");
        require(u.full, "full desired resident snapshot required");
        if (initialized)
        {
            require(u.revision >= last.revision, "revision regression");
            if (u.epoch == last.epoch)
            {
                require(u.cursor >= last.cursor, "out of order cursor");
                if (u.cursor == last.cursor)
                {
                    require(bytes == raw, "conflicting cursor payload");
                    return false;
                }
            }
            if (u.revision == last.revision)
                require(same_poses(u, last), "conflicting state at same revision");
        }
        last        = u;
        raw         = bytes;
        initialized = true;
        return true;
    }
};
} // namespace living
