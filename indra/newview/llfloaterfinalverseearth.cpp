// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
#include "llviewerprecompiledheaders.h"
#include "llfloaterfinalverseearth.h"
#include "llagent.h"
#include "llviewerregion.h"
#include "llbutton.h"
#include "lllineeditor.h"
#include "lltexteditor.h"
#include "llcoros.h"
#include "llcorehttputil.h"
#include "llsdutil.h"
#include "llfinalversepresentation.h"
#include "llappviewer.h"
#include "llfloaterfinalverseworkspace.h"
#include <sstream>

bool LLFloaterFinalverseEarth::postBuild()
{
    for(const char* action:{"preview","commit","reject","inspect","undo","lumi"}) {
        const std::string name(action);
        getChild<LLButton>(name)->setCommitCallback([this,name](LLUICtrl*,const LLSD&){run(name);});
    }
    getChild<LLButton>("visit")->setCommitCallback([this](LLUICtrl*,const LLSD&){
        auto region=gAgent.getRegion();const auto position=mDestination["position"];
        if(!region||mDestination["region_id"].asString()!=region->getRegionID().asString()||!position.isArray()||position.size()!=3)return;
        gAgent.teleportViaLocation(region->getOriginGlobal()+LLVector3d(position[0].asReal(),position[1].asReal(),position[2].asReal()));
    });
    return true;
}
void LLFloaterFinalverseEarth::onOpen(const LLSD&)
{
    auto region=gAgent.getRegion();const LLUUID current=region?region->getRegionID():LLUUID::null;
    if(mOwner!=gAgentID||mRegion!=current) {
        mOwner=gAgentID;mRegion=current;mPlan.clear();mStatus.clear();mDestination=LLSD();
        getChild<LLLineEditor>("handoff")->setText(std::string());getChild<LLLineEditor>("plan_id")->setText(std::string());
    }
    if(mBusy)return;
    if(!mPlan.empty()) {
        run("inspect");
        return;
    }
    getChild<LLTextEditor>("review")->setText("Create a proposal in Earth Intelligence, then paste its single-use handoff here. Preview does not create objects. Review source evidence and permissions before approving.\n\nCurrent region: "+(region?region->getName():"Enter a world first")+"\nYour avatar ID for the handoff: "+gAgentID.asString()+"\n\nTo revisit an experience, enter its saved native plan ID and choose Inspect.");buttons();
}
void LLFloaterFinalverseEarth::buttons()
{
    getChild<LLButton>("preview")->setEnabled(!mBusy);
    getChild<LLButton>("inspect")->setEnabled(!mBusy);
    for(const char* action:{"commit","reject"})getChild<LLButton>(action)->setEnabled(!mBusy&&mStatus=="prepared");
    for(const char* action:{"undo","visit"})getChild<LLButton>(action)->setEnabled(!mBusy&&mStatus=="committed");
    getChild<LLButton>("lumi")->setEnabled(!mBusy&&!mPlan.empty()&&llfinalverseWorldServices().citizen_interactive);
}
void LLFloaterFinalverseEarth::run(const std::string& action)
{
    auto region=gAgent.getRegion();if(mBusy||!region)return;
    const std::string cap=region->getCapability("FinalverseWorld");
    if(cap.empty()){getChild<LLTextEditor>("review")->setText(std::string("Enter a MutSea world with Earth Bridge enabled first."));return;}
    LLSD body;body["op"]="earth_bridge";body["action"]=action;
    if(action=="preview") {
        body["handoff"]=getChild<LLLineEditor>("handoff")->getText();
        getChild<LLLineEditor>("handoff")->setText(std::string());
        mStatus.clear();mPlan.clear();
    } else {
        auto saved=getChild<LLLineEditor>("plan_id")->getText();
        if(action=="inspect"&&LLUUID::validate(saved))mPlan=saved;
        if(!LLUUID::validate(mPlan)){getChild<LLTextEditor>("review")->setText(std::string("Enter a valid native plan ID to inspect an existing experience."));return;}body["plan_id"]=mPlan;
    }
    if(action=="lumi") {body=LLSD().with("op","citizen").with("command","experience").with("text",mPlan);}
    const LLUUID actor=gAgentID,region_id=region->getRegionID();
    auto handle=getHandle();mBusy=true;buttons();
    getChild<LLTextEditor>("review")->setText(std::string(action=="preview"?"Validating the handoff, pinned assets, permissions and placement…":"Checking the authoritative world transaction…"));
    LLCoros::instance().launch("FinalverseEarthBridge",[handle,body,cap,actor,region_id,action]{
        LLCoreHttpUtil::HttpCoroutineAdapter adapter("FinalverseEarthBridge",LLCore::HttpRequest::DEFAULT_POLICY_ID);
        auto request=std::make_shared<LLCore::HttpRequest>();auto options=std::make_shared<LLCore::HttpOptions>();
        options->setTimeout(25);options->setRetries(0);
        auto reply=adapter.postJsonAndSuspend(request,cap,body,options);
        // LLTextEditor and floater controls acquire non-coroutine-aware locks.
        // Keep network suspension in this coroutine and apply every UI result
        // through the existing main-loop queue, including error/session paths.
        LLAppViewer::instance()->postToMainCoro([handle,reply,actor,region_id,cap,action]{
        llassert(LLCoros::on_main_coro());
        auto self=dynamic_cast<LLFloaterFinalverseEarth*>(handle.get());if(!self)return;
        self->mBusy=false;
        auto current=gAgent.getRegion();
        if(gAgentID!=actor||!current||current->getRegionID()!=region_id||current->getCapability("FinalverseWorld")!=cap){
            self->mStatus.clear();self->mPlan.clear();self->getChild<LLTextEditor>("review")->setText(std::string("Your world session changed. Reopen the experience in the destination region before taking another action."));self->buttons();return;
        }
        if(reply["ok"].isBoolean()&&reply["ok"].asBoolean()&&reply["result"].isMap()&&(action=="lumi"||reply["result"]["fsip"].isMap())) {
            if(action=="lumi") {
                llfinalverseMessage("conversation", "Lumi", reply["result"]["answer"].asString());
                self->getChild<LLTextEditor>("review")->setText("Lumi · experience review\n\n"+reply["result"]["answer"].asString()+"\n\nThis conversation grants no world-edit permission. Inspect the proposal and approve separately.");
            }
            else self->render(reply["result"]);
        }
        else {
            // An unconfirmed commit must not leave Apply enabled for a retry.
            if(action=="commit"||action=="undo"||action=="reject")self->mStatus="unknown";
            const std::string fallback=action=="preview"
                ?"Preview was not confirmed. No creation was authorized. Check the experience in Earth Intelligence and revoke its handoff before issuing another."
                :"No result was confirmed. Inspect the saved plan ID before taking another action.";
            self->getChild<LLTextEditor>("review")->setText(reply.has("error")?reply["error"].asString():fallback);
        }
        self->buttons();
        });
    });
}
void LLFloaterFinalverseEarth::render(const LLSD& result)
{
    const LLSD f=result["fsip"];if(!f.isMap())return;
    mPlan=result["plan_id"].asString();mStatus=f["status"].asString();mDestination=f["destination"];
    llfinalverseMessage("activity", "Earth → World", f["title"].asString()+" · "+mStatus+(result.has("receipt_sync")?" · receipt "+result["receipt_sync"].asString():""));
    getChild<LLLineEditor>("plan_id")->setText(mPlan);
    std::ostringstream text;
    text<<f["title"].asString()<<"\n"<<(f["layout"].asString()=="virtual"?"Virtual world · no geographic binding":"Schematic exhibition · archival source evidence")<<"\n\n";
    text<<f["context"]["description"].asString()<<"\n\n";
    if(f["source_workspace"].isMap())text<<"Source workspace: "<<f["source_workspace"]["title"].asString()<<"\nArtifact: "<<f["source_workspace"]["artifact_id"].asString()<<"\n\n";
    text<<"AI proposal\n"<<f["narrative"].asString()<<"\n\nPermissions: create your own approved place; inspect and undo it. Native parcel permissions still apply. No scripts.\n\n";
    if(result.has("summary"))text<<result["summary"].asString()<<"\n"<<result["siting"].asString()<<"\n\n";
    for(const auto& p:llsd::inArray(f["provenance"]))text<<(p["generated"].asBoolean()?"Generated geometry: ":"Source records: ")<<p["attribution"].asString()<<"\nLicense: "<<p["license"].asString()<<"\n"<<p["source"].asString()<<"\n\n";
    for(const auto& exhibit:llsd::inArray(f["exhibits"]))text<<exhibit["description"].asString()<<"\n\n";
    text<<"Status: "<<mStatus<<"\nManifest: "<<f["manifest_sha256"].asString()<<"\n";
    if(mStatus=="prepared")text<<"Approval expires: "<<f["expires_at"].asString()<<"\n";
    if(result["receipt"].isMap())text<<"WorldLine: "<<result["receipt"]["worldline_operation_id"].asString()<<"\nBound exhibits/assets: "<<result["receipt"]["bindings"].size()<<"\nEarth receipt: "<<result["receipt_sync"].asString()<<"\n";
    getChild<LLTextEditor>("review")->setText(text.str());
}

#include "llfinalverselivingcontract.h"
#include "llcommandhandler.h"
#include "llfloaterreg.h"
#include "llsdjson.h"
#include "llurldispatcher.h"
#include "llweb.h"
#include "lluri.h"
namespace
{
LLSD livingPlacement;
LLUUID livingActor,livingSession,livingRegion;
class LivingEarthHandler : public LLCommandHandler
{
public:
    LivingEarthHandler():LLCommandHandler("living_earth",UNTRUSTED_THROTTLE){}
    bool handle(const LLSD& path,const LLSD& query,const std::string&,LLMediaCtrl*) override
    {
        if(path.size()!=1||path[0].asString()!="open"||!llfinalverse_living::keys(query,{"request"})||!query["request"].isString()||query["request"].asString().size()>8192)return false;
        try{
            const auto h=LlsdFromJson(boost::json::parse(query["request"].asString()));
            if(!llfinalverse_living::handoff(h)){LL_WARNS("LivingEarth")<<"Rejected invalid world handoff envelope"<<LL_ENDL;return false;}
            LLFloaterReg::showInstance("finalverse_living_earth",h);
            return true;
        }catch(...){return false;}
    }
} livingEarthHandler;
}
LLSD llfinalverseLivingPlacement()
{
    const auto region=gAgent.getRegion();
    if(gDisconnected||!region||LLAppViewer::instance()->logoutRequestSent()||gAgentID!=livingActor||gAgent.getSessionID()!=livingSession||region->getRegionID()!=livingRegion){livingPlacement=LLSD();return LLSD();}
    return livingPlacement;
}
bool LLFloaterFinalverseLivingEarth::postBuild()
{
    getChild<LLButton>("enter")->setCommitCallback([this](LLUICtrl*,const LLSD&){
        if(!llfinalverse_living::handoff(mHandoff))return;
        const auto c=mHandoff["connection"];auto start=c["start"].asString().substr(4);std::vector<std::string> parts;std::stringstream in(start);std::string part;while(std::getline(in,part,'&'))parts.push_back(part);
        if(parts.size()!=4)return;
        LLURI uri(c["login_uri"].asString());
        const std::string target="x-grid-location-info://"+uri.authority()+"/region/"+LLURI::escape(parts[0])+"/"+parts[1]+"/"+parts[2]+"/"+parts[3];
        LLURLDispatcher::dispatch(target,LLCommandHandler::NAV_TYPE_CLICKED,nullptr,false);
        display("Destination selected in the native login/world flow. Sign in normally, then Verify arrival. A discovery link grants no world access.");
    });
    getChild<LLButton>("verify")->setCommitCallback([this](LLUICtrl*,const LLSD&){verify(false);});
    getChild<LLButton>("receipt")->setCommitCallback([this](LLUICtrl*,const LLSD&){verify(true);});
    getChild<LLButton>("storm")->setCommitCallback([](LLUICtrl*,const LLSD&){LLFloaterReg::showInstance("finalverse_storm");});
    getChild<LLButton>("return")->setCommitCallback([this](LLUICtrl*,const LLSD&){
        if(!llfinalverse_living::handoff(mHandoff))return;
        const auto original=boost::json::serialize(LlsdToJson(mHandoff["return_to"]["destination"]));
        auto origin=mHandoff["return_to"]["origin"].asString();if(origin.back()=='/')origin.pop_back();
        LLWeb::loadURLExternal(origin+"/#earth_return="+LLURI::escape(original));
        display("Opened the original Earth destination. Your world login stays in the native Viewer; reconnect your publication read grant in the browser if needed.");
    });
    return true;
}
void LLFloaterFinalverseLivingEarth::onOpen(const LLSD& key)
{
    if(llfinalverse_living::handoff(key)){mHandoff=key;livingPlacement=LLSD();}
    display("Review this geographic destination before entering. Existing native sign-in and permissions apply. After login, verify the authoritative binding before opening the combined FinalStorm view.");
}
void LLFloaterFinalverseLivingEarth::display(const std::string& message)
{
    const bool valid=llfinalverse_living::handoff(mHandoff);
    std::ostringstream out;out<<message<<"\n\n";
    if(valid)out<<"World: "<<mHandoff["world_binding"]["world_id"].asString()<<"\nRegion: "<<mHandoff["world_binding"]["region_id"].asString()<<"\nLogin service: "<<mHandoff["connection"]["login_uri"].asString()<<"\nStanding location: "<<mHandoff["connection"]["start"].asString()<<"\n\nPublication: "<<mHandoff["publication"]["version"].asString()<<"\nBinding: "<<mHandoff["world_binding"]["version"].asString()<<"\n\nNo account, password or capability is carried by this link. Geographic source provenance remains in Earth. Live edits remain under MutSea authority.";
    getChild<LLTextEditor>("review")->setText(out.str());
    for(auto name:{"enter","verify","receipt","return"})getChild<LLButton>(name)->setEnabled(valid&&!mBusy);
    getChild<LLButton>("storm")->setEnabled(valid&&!mBusy&&llfinalverseLivingPlacement().isMap());
}
void LLFloaterFinalverseLivingEarth::verify(bool arrival)
{
    if(mBusy||!llfinalverse_living::handoff(mHandoff))return;
    const auto region=gAgent.getRegion();if(!region||gAgentID.isNull()||region->getRegionID().asString()!=mHandoff["world_binding"]["region_id"].asString()){display("Sign in to the linked world first. The current region does not match the handoff.");return;}
    const auto cap=region->getCapability("FinalverseWorld");if(cap.empty()){display("This region has no authenticated geographic world capability.");return;}
    LLSD body;body["op"]=arrival?"geo_enter":"geo_binding";
    if(arrival){if(!llfinalverseLivingPlacement().isMap()){display("Verify the binding before recording arrival.");return;}body["binding_version"]=mHandoff["world_binding"]["version"];const auto address=mHandoff["world_entry"]["address"].asString();if(address.find("usap://spatialdb/entity/")==0)body["spatialdb_entity"]=address.substr(std::string("usap://spatialdb/entity/").size());else{body["latitude"]=mHandoff["world_entry"]["anchor"]["latitude_degrees"];body["longitude"]=mHandoff["world_entry"]["anchor"]["longitude_degrees"];}}
    const auto actor=gAgentID,session=gAgent.getSessionID(),regionID=region->getRegionID();const auto handoff=mHandoff;const auto handle=getHandle();mBusy=true;display("Checking the authenticated world binding…");
    LLCoros::instance().launch("FinalverseLivingEarth",[handle,body,cap,actor,session,regionID,handoff,arrival]{
        LLCoreHttpUtil::HttpCoroutineAdapter adapter("FinalverseLivingEarth",LLCore::HttpRequest::DEFAULT_POLICY_ID);auto request=std::make_shared<LLCore::HttpRequest>();auto options=std::make_shared<LLCore::HttpOptions>();options->setTimeout(15);options->setRetries(0);auto headers=std::make_shared<LLCore::HttpHeaders>();headers->append("Connection","close");const auto reply=adapter.postJsonAndSuspend(request,cap,body,options,headers);
        LLAppViewer::instance()->postToMainCoro([handle,reply,cap,actor,session,regionID,handoff,arrival]{
            auto self=dynamic_cast<LLFloaterFinalverseLivingEarth*>(handle.get());if(!self)return;self->mBusy=false;const auto current=gAgent.getRegion();
            if(!current||gAgentID!=actor||gAgent.getSessionID()!=session||current->getRegionID()!=regionID||current->getCapability("FinalverseWorld")!=cap||self->mHandoff!=handoff){livingPlacement=LLSD();self->display("Session or destination changed. Verify the new session before continuing.");return;}
            const auto result=reply["result"],b=result["binding"],p=b["publication"],expected=handoff["publication"];
            if(!reply["ok"].isBoolean()||!reply["ok"].asBoolean()){livingPlacement=LLSD();self->display("The authenticated world check was not confirmed. No entry receipt or mutation is assumed; retry verification when the service recovers.");return;}
            if(arrival){const auto receipt=result["receipt"];const bool arrived=receipt["schema_version"].asString()=="mutsea.geographic-entry-receipt/v1"&&receipt["status"].asString()=="arrived"&&receipt["actor_id"].asString()==actor.asString()&&receipt["region_id"].asString()==regionID.asString()&&receipt["binding_version"]==handoff["world_binding"]["version"];
                self->display(arrived?"Authenticated arrival receipt received. Native world permissions and WorldLine remain authoritative.":"The server did not confirm arrival at this destination. Navigate to the standing place, then record arrival again.");return;}
            if(result["schema_version"].asString()!="mutsea.geographic-binding-view/v1"||!result["terrain_matches_issued"].isBoolean()||!result["terrain_matches_issued"].asBoolean()||result["recovery_required"].asBoolean()||b["world_id"]!=handoff["world_binding"]["world_id"]||b["region"]["region_id"].asString()!=regionID.asString()||b["version"]!=handoff["world_binding"]["version"]||p["graph_id"]!=expected["graph_id"]||p["version"]!=expected["version"]||p["root_sha256"]!=expected["root_sha256"]||p["frame_id"]!=expected["frame_id"]||p["frame_origin"]!=expected["origin"]){livingPlacement=LLSD();self->display("The authoritative binding or publication differs from the handoff. Rediscover this world in Earth; the geographic viewport remains disabled.");return;}
            LLSD placement;placement["schema"]="finalverse.desktop-geography/v1";for(auto field:{"graph_id","version","root_sha256","frame_id"})placement[field]=p[field];placement["origin"]=p["frame_origin"];placement["region_id"]=regionID.asString();placement["world_id"]=b["world_id"];placement["binding_version"]=b["version"];placement["region_to_ecef"]=b["transform"]["region_to_ecef"];placement["region_to_publication"]=b["transform"]["region_to_publication"];
            livingPlacement=placement;livingActor=actor;livingRegion=regionID;livingSession=session;
            LL_INFOS("LivingEarth")<<"Verified binding "<<b["version"].asString()<<" publication "<<p["version"].asString()<<LL_ENDL;
            self->display("Arrival verified against the current authenticated session, terrain, binding and immutable publication. Open FinalStorm for the combined geographic and live object view.");
        });
    });
}
