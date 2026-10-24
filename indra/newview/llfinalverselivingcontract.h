// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
#ifndef LL_FINALVERSE_LIVING_CONTRACT_H
#define LL_FINALVERSE_LIVING_CONTRACT_H
#include "llsd.h"
#include "lluuid.h"
#include <cmath>
#include <regex>
#include <set>
namespace llfinalverse_living
{
inline bool keys(const LLSD& v, std::initializer_list<const char*> expected)
{
    if (!v.isMap() || v.size()!=expected.size()) return false;
    for (auto key:expected) if(!v.has(key)) return false;
    return true;
}
inline bool number(const LLSD& v) {return (v.isReal()||v.isInteger())&&std::isfinite(v.asReal());}
inline bool digest(const LLSD& v) {return v.isString()&&std::regex_match(v.asString(),std::regex("[0-9a-f]{64}"));}
inline bool uuid(const LLSD& v) {return v.isString()&&LLUUID::validate(v.asString())&&LLUUID(v.asString()).notNull()&&LLUUID(v.asString()).asString()==v.asString();}
inline bool position(const LLSD& v)
{
    return keys(v,{"longitude_degrees","latitude_degrees","height_metres"})&&number(v["longitude_degrees"])&&number(v["latitude_degrees"])&&number(v["height_metres"])&&std::abs(v["longitude_degrees"].asReal())<=180&&std::abs(v["latitude_degrees"].asReal())<=90;
}
inline bool origin(const LLSD& v)
{
    if(!v.isString()||v.asString().size()>256) return false;
    // Canonical credential-free origins only, including explicit loopback development.
    return std::regex_match(v.asString(),std::regex("(https://[a-z0-9]([a-z0-9.-]*[a-z0-9])?|(http://(127\\.0\\.0\\.1|localhost|\\[::1\\])))(:[1-9][0-9]{0,4})?/?"));
}
inline bool address(const LLSD& v) {return v.isString()&&std::regex_match(v.asString(),std::regex("usap://[a-z][a-z0-9-]{0,62}/entity/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"))&&uuid(v.asString().substr(v.asString().size()-36));}
inline bool destination(const LLSD& d)
{
    if(d["schema_version"].asString()!="earth.navigation/v1")return false;
    return d["kind"].asString()=="geographic" ? keys(d,{"schema_version","kind","position"})&&position(d["position"]) : d["kind"].asString()=="entity"&&keys(d,{"schema_version","kind","address"})&&address(d["address"]);
}
inline bool handoff(const LLSD& h)
{
    if(!keys(h,{"schema_version","request_id","world_binding","world_entry","publication","connection","return_to"})||h["schema_version"].asString()!="earth.world-handoff/v1"||!uuid(h["request_id"])) return false;
    const auto b=h["world_binding"],e=h["world_entry"],p=h["publication"],c=h["connection"],r=h["return_to"];
    if(!keys(b,{"schema_version","address","authority","world_id","region_id","anchor","frame","heading_degrees","scale","version"})||b["schema_version"].asString()!="earth.world-binding/v1"||b["authority"].asString()!="mutsea"||!uuid(b["world_id"])||!uuid(b["region_id"])||!digest(b["version"])||!position(b["anchor"])||!keys(b["frame"],{"axes","unit"})||b["frame"]["unit"].asString()!="metre"||b["frame"]["axes"].size()!=3||b["frame"]["axes"][0].asString()!="east"||b["frame"]["axes"][1].asString()!="north"||b["frame"]["axes"][2].asString()!="up")return false;
    if(b["address"].asString()!="usap://mutsea/entity/"+b["region_id"].asString()||!number(b["heading_degrees"])||b["heading_degrees"].asReal()<0||b["heading_degrees"].asReal()>=360||!number(b["scale"])||b["scale"].asReal()<=0||b["scale"].asReal()>1e6)return false;
    if(!keys(e,{"schema_version","address","authority","world_id","region_id","anchor","authorization","binding_relation"})||e["schema_version"].asString()!="earth.world-entry/v1"||e["authorization"].asString()!="required"||!address(e["address"])||!position(e["anchor"])||e["authority"]!=b["authority"]||e["world_id"]!=b["world_id"]||e["region_id"]!=b["region_id"]||!std::regex_match(e["binding_relation"].asString(),std::regex("[a-z_]{1,64}")))return false;
    if(!keys(p,{"graph_id","version","root_sha256","frame_id","origin","axes","unit"})||!uuid(p["graph_id"])||!digest(p["version"])||!digest(p["root_sha256"])||!position(p["origin"])||!std::regex_match(p["frame_id"].asString(),std::regex("[a-zA-Z0-9._/-]{1,160}"))||p["unit"].asString()!="metre"||p["axes"].size()!=3||p["axes"][0].asString()!="east"||p["axes"][1].asString()!="up"||p["axes"][2].asString()!="south")return false;
    if(!keys(c,{"protocol","login_uri","start"})||c["protocol"].asString()!="opensim.login/v1"||!origin(c["login_uri"])||c["start"].asString().size()>256)return false;
    std::smatch parts;const auto start=c["start"].asString();
    if(!std::regex_match(start,parts,std::regex("uri:([^&\\x00-\\x1f]{1,96})&([0-9.]+)&([0-9.]+)&(-?[0-9.]+)")))return false;
    try{for(int i=2;i<=4;++i){size_t end=0;const auto value=std::stod(parts[i].str(),&end);if(end!=parts[i].length()||!std::isfinite(value)||(i<4&&(value<0||value>8192))||(i==4&&std::abs(value)>10000))return false;}}catch(...){return false;}
    return keys(r,{"origin","destination"})&&origin(r["origin"])&&destination(r["destination"]);
}
}
#endif
