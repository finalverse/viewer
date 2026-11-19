// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
#include "llviewerprecompiledheaders.h"
#include "llfloaterfinalversenative.h"
#include "finalverse/connection.h"
#include "finalverse/fvp.h"
#include "finalverse/storm.h"
#include "gltf/asset.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "lldynamictexture.h"
#include "llfloaterreg.h"
#include "llrender.h"
#include "lltextbox.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llimage.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llviewertexture.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/sha.h>
#include <glm/gtc/type_ptr.hpp>
namespace {
using Clock=std::chrono::steady_clock;
void need(bool yes,const char *message){if(!yes)throw std::runtime_error(message);}
std::string digest(const fv_digest &d) {
 const char *hex="0123456789abcdef";std::string s;for(auto b:d.bytes){s+=hex[b>>4];s+=hex[b&15];}return s;
}
std::string identity(const fv_uuid &u) {
 LLUUID id;std::memcpy(id.mData,u.bytes,16);return id.asString();
}
std::vector<uint8_t> readBytes(const std::filesystem::path &path,size_t bound) {
 std::ifstream in(path,std::ios::binary|std::ios::ate);
 need(in.good() && in.tellg()>0 && in.tellg()<=static_cast<std::streamoff>(bound),"bounded local file");
 std::vector<uint8_t> data(static_cast<size_t>(in.tellg()));in.seekg(0);
 in.read(reinterpret_cast<char *>(data.data()),static_cast<std::streamsize>(data.size()));
 need(in.good(),"local file read");return data;
}
uint64_t unsignedJson(const boost::json::value &v) {
 if(v.is_uint64())return v.as_uint64();
 if(v.is_int64()){need(v.as_int64()>=0,"unsigned frame value");return static_cast<uint64_t>(v.as_int64());}
 need(v.is_string(),"unsigned frame value");const auto &text=v.as_string();uint64_t result=0;
 auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
 need(parsed.ec==std::errc()&&parsed.ptr==text.data()+text.size(),"unsigned frame decimal");return result;
}
struct Shape {
 std::string hash;
 std::shared_ptr<const std::string> glb;
 fv_pose pose{{0,0,0},{0,0,0,1},{1,1,1}};
 bool resident=false;
 std::string entity,asset,version;
};
struct World {
 fv_world_info info{};
 std::vector<fv_resident_info> residents;
 std::vector<Shape> shapes;
 std::vector<fv_event_info> history;
};
struct StormFrame {
 std::string world,definition;
 std::vector<uint8_t> rgba;
 uint64_t number=0,triangles=0,sourceRevision=0;
 bool interpolating=false;
 std::map<std::string,std::array<double,3>> registeredResidents;
 Clock::time_point received;
};
struct Observation {
 fv_stream_stamp stamp{};
 std::vector<World> worlds;
 Clock::time_point received;
 std::shared_ptr<const StormFrame> storm;
 bool authoredPreview=false;
};
struct InteractionStatus {
 std::string id,world,region,resident,stage,detail,revision,command,proposal;
 bool requiresApproval=false;
};
struct RenderAdmission {
 std::string world,region,definition,entity,publication,command;
 uint64_t revision=0;
 bool operator==(const RenderAdmission &other)const {
  return world==other.world&&region==other.region&&definition==other.definition&&entity==other.entity&&publication==other.publication&&command==other.command&&revision==other.revision;
 }
};
struct Worker {
 std::atomic<bool> stop{false};
 std::mutex mutex,connectionMutex;
 std::condition_variable wake;
 std::shared_ptr<const Observation> latest;
 fv_connection *active=nullptr;
 std::thread thread;
 std::string socket,grant,cache,status="Connecting…";
 std::string stormDirectory,stormDefinition,fallbackStormDefinition,admissionFile,previewFile,admittedWorld,interactionDirectory;
 bool authoredPreview=false;
 std::unique_ptr<RenderAdmission> acceptedAdmission;
 static bool admissionBinding(const RenderAdmission &a,const std::vector<World> &worlds,bool history) {
  for(const auto &w:worlds) {
   if(identity(w.info.world)!=a.world||identity(w.info.region)!=a.region||w.info.revision<a.revision)continue;
   if(!std::any_of(w.shapes.begin(),w.shapes.end(),[&](const auto &shape){return !shape.resident&&shape.entity==a.entity&&shape.version==a.publication;}))return false;
   if(!history)return true;
   for(const auto &event:w.history)for(uint32_t i=0;i<std::min<uint32_t>(event.command_count,16);i++)if(identity(event.commands[i])==a.command)return true;
  }
  return false;
 }
 void admission(const std::vector<World> &worlds) {
  std::string effective=fallbackStormDefinition,world;
  if(acceptedAdmission&&admissionBinding(*acceptedAdmission,worlds,false)){effective=acceptedAdmission->definition;world=acceptedAdmission->world;}
  if(!admissionFile.empty())try {
   namespace fs=std::filesystem;const fs::path path(admissionFile);struct stat parent{};
   need(path.is_absolute()&&lstat(path.parent_path().c_str(),&parent)==0&&S_ISDIR(parent.st_mode)&&parent.st_uid==getuid()&&(parent.st_mode&077)==0,"private admission directory");
   struct FileHandle {int fd=-1;~FileHandle(){if(fd>=0)::close(fd);}} handle;
   handle.fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);const int fd=handle.fd;
   need(fd>=0,"admission unavailable");
   struct stat metadata{};need(fstat(fd,&metadata)==0&&S_ISREG(metadata.st_mode)&&metadata.st_uid==getuid()&&(metadata.st_mode&0777)==0600&&metadata.st_size>0&&metadata.st_size<=4096,"private bounded admission file");
   std::string raw(static_cast<size_t>(metadata.st_size),'\0');size_t offset=0;
   while(offset<raw.size()){const auto n=::read(fd,raw.data()+offset,raw.size()-offset);need(n>0,"admission read");offset+=static_cast<size_t>(n);}
   auto j=boost::json::parse(raw).as_object();need(j.size()==8,"admission keys");
   auto field=[&](const char *name,size_t bound){auto it=j.find(name);need(it!=j.end()&&it->value().is_string(),"admission field");auto text=std::string(it->value().as_string());need(text.size()<=bound,"admission field bound");return text;};
   need(field("schema",64)=="finalverse.render-admission/0.1","admission schema");RenderAdmission candidate;
   candidate.world=field("world",36);candidate.region=field("region",36);candidate.definition=field("definition_sha256",64);candidate.entity=field("source_entity",36);candidate.publication=field("publication_version",64);candidate.command=field("command_id",36);
   for(const auto *id:{&candidate.world,&candidate.region,&candidate.entity,&candidate.command})need(LLUUID::validate(*id)&&*id!="00000000-0000-0000-0000-000000000000","admission UUID");
   for(const auto *sha:{&candidate.definition,&candidate.publication})need(sha->size()==64&&sha->find_first_not_of("0123456789abcdef")==std::string::npos,"admission digest");
   const auto revision=field("committed_revision",20);candidate.revision=unsignedJson(boost::json::value(revision));need(candidate.revision>0,"admission revision");
   const bool retained=acceptedAdmission&&candidate==*acceptedAdmission;
   if(admissionBinding(candidate,worlds,!retained)){
    effective=candidate.definition;world=candidate.world;
    if(!retained){acceptedAdmission=std::make_unique<RenderAdmission>(candidate);LL_INFOS("FinalverseNative")<<"Render admission verified world="<<candidate.world<<" region="<<candidate.region<<" definition="<<candidate.definition<<" committed_revision="<<candidate.revision<<LL_ENDL;}
   }
  }catch(const std::exception &){/* An invalid replacement cannot grant a new pin. */}
  authoredPreview=false;
  if(!previewFile.empty()&&acceptedAdmission&&admissionBinding(*acceptedAdmission,worlds,false))try {
   namespace fs=std::filesystem;const fs::path path(previewFile);struct stat parent{};
   need(path.is_absolute()&&lstat(path.parent_path().c_str(),&parent)==0&&S_ISDIR(parent.st_mode)&&parent.st_uid==getuid()&&(parent.st_mode&077)==0,"private preview directory");
   const int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
   struct PreviewHandle{int fd;~PreviewHandle(){if(fd>=0)::close(fd);}} handle{fd};struct stat metadata{};
   need(fd>=0&&fstat(fd,&metadata)==0&&S_ISREG(metadata.st_mode)&&metadata.st_uid==getuid()&&(metadata.st_mode&0777)==0600&&metadata.st_size>0&&metadata.st_size<=4096,"private preview file");
   std::string raw(static_cast<size_t>(metadata.st_size),'\0');size_t offset=0;while(offset<raw.size()){const auto n=::read(fd,raw.data()+offset,raw.size()-offset);need(n>0,"preview read");offset+=static_cast<size_t>(n);}
   auto j=boost::json::parse(raw).as_object();need(j.size()==8,"preview keys");
   auto field=[&](const char *key,size_t bound){auto it=j.find(key);need(it!=j.end()&&it->value().is_string(),"preview field");auto value=std::string(it->value().as_string());need(value.size()<=bound,"preview field bound");return value;};
   need(field("schema",64)=="finalverse.render-preview/0.1"&&field("base_definition_sha256",64)==effective,"preview base pin");
   const auto previewWorld=field("world",36),previewRegion=field("region",36),definition=field("definition_sha256",64),request=field("request_id",36);
   need(previewWorld==acceptedAdmission->world&&previewRegion==acceptedAdmission->region&&definition.size()==64&&definition.find_first_not_of("0123456789abcdef")==std::string::npos&&LLUUID::validate(request)&&request!="00000000-0000-0000-0000-000000000000","preview scope/digest");
   need(j.at("preview_only").is_bool()&&j.at("preview_only").as_bool(),"preview classification");
   const auto sourceRevision=unsignedJson(boost::json::value(field("source_revision",20)));
   for(const auto &w:worlds)if(identity(w.info.world)==previewWorld&&identity(w.info.region)==previewRegion&&sourceRevision<=w.info.revision&&w.info.revision-sourceRevision<=64){effective=definition;world=previewWorld;authoredPreview=true;break;}
  }catch(const std::exception &){/* Preview failure never admits a world mutation. */}
  stormDefinition=std::move(effective);admittedWorld=std::move(world);
 }
 std::string queuedRequest,requestId,requestWorld,requestRegion,requestResident;
 std::shared_ptr<const InteractionStatus> interactionStatus;
 bool submitInteraction(std::string request,std::string id,std::string world,std::string region,std::string resident) {
  std::lock_guard lock(mutex);
  if(interactionDirectory.empty()||!queuedRequest.empty()||request.size()>4096)return false;
  queuedRequest=std::move(request);requestId=std::move(id);requestWorld=std::move(world);requestRegion=std::move(region);requestResident=std::move(resident);
  auto pending=std::make_shared<InteractionStatus>();pending->id=requestId;pending->world=requestWorld;pending->region=requestRegion;pending->resident=requestResident;
  pending->stage="pending";pending->detail="Waiting for the owning service. No world change is confirmed.";interactionStatus=pending;
  wake.notify_all();return true;
 }
 void interaction() {
  if(interactionDirectory.empty())return;
  try {
   namespace fs=std::filesystem;fs::path root(interactionDirectory);
   need(root.is_absolute()&&fs::is_directory(fs::symlink_status(root)),"private interaction directory");
   need((fs::status(root).permissions()&(fs::perms::group_all|fs::perms::others_all))==fs::perms::none,"private interaction permissions");
   std::string queued,id,world,region,resident;
   {std::lock_guard lock(mutex);queued.swap(queuedRequest);id=requestId;world=requestWorld;region=requestRegion;resident=requestResident;}
   if(!queued.empty()) {
    auto tmp=root/("request-"+id+".part");
    {std::ofstream file(tmp,std::ios::binary);file<<queued;need(file.good(),"interaction request write");}
    fs::permissions(tmp,fs::perms::owner_read|fs::perms::owner_write);
    fs::rename(tmp,root/"request.json");
   }
   if(id.empty()||!fs::is_regular_file(fs::symlink_status(root/"status.json")))return;
   auto bytes=readBytes(root/"status.json",4096);
   auto j=boost::json::parse(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())).as_object();
   auto field=[&](const char *name,size_t bound){auto it=j.find(name);need(it!=j.end()&&it->value().is_string(),"interaction status field");auto value=std::string(it->value().as_string());need(value.size()<=bound,"interaction status field bound");return value;};
   need(field("schema",64)=="finalverse.viewer-status/0.1","interaction status schema");
   if(field("id",36)!=id||field("world",36)!=world||field("region",36)!=region)return;
   auto next=std::make_shared<InteractionStatus>();next->id=id;next->world=world;next->region=region;next->resident=resident;
   next->stage=field("stage",16);need(next->stage=="pending"||next->stage=="accepted"||next->stage=="rejected"||next->stage=="committed"||next->stage=="reconciled","interaction status stage");
   next->detail=field("detail",512);next->revision=field("committed_revision",20);
   if(auto it=j.find("command_id");it!=j.end()&&it->value().is_string())next->command=std::string(it->value().as_string());
   if(auto it=j.find("proposal_id");it!=j.end()&&it->value().is_string())next->proposal=std::string(it->value().as_string());
   if(auto it=j.find("requires_approval");it!=j.end()&&it->value().is_bool())next->requiresApproval=it->value().as_bool();
   need(next->command.size()<=36&&next->proposal.size()<=36,"interaction reference bounds");
   if(next->requiresApproval)need(next->stage=="accepted"&&LLUUID::validate(next->proposal),"concrete review proposal required");
   {std::lock_guard lock(mutex);if(requestId==id)interactionStatus=next;}
  }catch(const std::exception &e){
   std::lock_guard lock(mutex);auto failure=std::make_shared<InteractionStatus>();failure->id=requestId;failure->world=requestWorld;failure->region=requestRegion;failure->resident=requestResident;failure->stage="pending";failure->detail="Interaction channel unavailable. Commit status is unknown; do not repeat approval.";interactionStatus=failure;
  }
 }
 Worker(std::string a,std::string b,std::string c):socket(std::move(a)),grant(std::move(b)),cache(std::move(c)) {
  const char *directory=std::getenv("FINALVERSE_STORM_FRAMES"),*definition=std::getenv("FINALVERSE_STORM_DEFINITION_SHA256");
  if(directory&&definition){stormDirectory=directory;stormDefinition=definition;fallbackStormDefinition=definition;}
  if(const char *admission=std::getenv("FINALVERSE_STORM_ADMISSION"))admissionFile=admission;
  if(const char *preview=std::getenv("FINALVERSE_STORM_PREVIEW"))previewFile=preview;
  if(const char *interaction=std::getenv("FINALVERSE_WORLD_INTERACTION_DIR"))interactionDirectory=interaction;
  thread=std::thread([this]{run();});
 }
 std::shared_ptr<const StormFrame> readStormFrame() {
  if(stormDirectory.empty())return nullptr;
  try {
   namespace fs=std::filesystem;fs::path directory(stormDirectory);
   need(directory.is_absolute()&&fs::is_directory(fs::symlink_status(directory)),"private Storm frame directory");
   need((fs::status(directory).permissions()&(fs::perms::group_all|fs::perms::others_all))==fs::perms::none,"private Storm frame permissions");
   for(const auto *name:{"frame.ppm","frame.json"}){
    auto path=directory/name;need(fs::is_regular_file(fs::symlink_status(path)),"regular Storm frame");
    auto age=fs::file_time_type::clock::now()-fs::last_write_time(path);
    need(age>=std::chrono::seconds(-1)&&age<=std::chrono::seconds(2),"stale Storm frame");
   }
   auto metadata=readBytes(directory/"frame.json",4096);
   auto json=boost::json::parse(std::string(reinterpret_cast<const char *>(metadata.data()),metadata.size()));
   auto report=LlsdFromJson(json);
   need(report["schema"].asString()=="finalverse.storm-view-frame/0.1"&&report["definition_sha256"].asString()==stormDefinition,"Storm frame definition pin");
   auto bytes=readBytes(directory/"frame.ppm",1280*720*3+128);uint8_t hash[32];SHA256(bytes.data(),bytes.size(),hash);
   fv_digest expected{};std::copy(hash,hash+32,expected.bytes);need(digest(expected)==report["pixels_sha256"].asString(),"Storm pixel hash");
   std::istringstream header(std::string(reinterpret_cast<const char *>(bytes.data()),std::min<size_t>(bytes.size(),128)));
   std::string magic;int width=0,height=0,maximum=0;header>>magic>>width>>height>>maximum;header.get();
   auto offset=header.tellg();need(magic=="P6"&&width==1280&&height==720&&maximum==255&&offset>0&&bytes.size()==static_cast<size_t>(offset)+1280*720*3,"Storm PPM dimensions");
   auto frame=std::make_shared<StormFrame>();frame->world=report["world"].asString();frame->definition=stormDefinition;
   need(admittedWorld.empty()||frame->world==admittedWorld,"admitted frame world");
   if(const auto *active=json.as_object().if_contains("interpolation_active")){need(active->is_bool(),"frame interpolation flag");frame->interpolating=active->as_bool();}
   frame->number=unsignedJson(json.at("frame"));frame->triangles=unsignedJson(json.at("triangles"));
   frame->sourceRevision=unsignedJson(json.at("source_revision"));
   if(report["registered_residents"].isArray()&&report["registered_residents"].size()<=16) {
    for(const auto &entry:llsd::inArray(report["registered_residents"])) {
     const auto id=entry["resident"].asString();const auto &p=entry["position"];
     if(!LLUUID::validate(id)||!p.isArray()||p.size()!=3)continue;
     std::array<double,3> position{p[0].asReal(),p[1].asReal(),p[2].asReal()};
     if(std::all_of(position.begin(),position.end(),[](double x){return std::isfinite(x)&&std::abs(x)<=10000000.;}))frame->registeredResidents.emplace(id,position);
    }
   }
   frame->received=Clock::now();frame->rgba.resize(1280*720*4);
   for(size_t i=0;i<1280*720;i++){for(size_t k=0;k<3;k++)frame->rgba[i*4+k]=bytes[static_cast<size_t>(offset)+i*3+k];frame->rgba[i*4+3]=255;}
   return frame;
  }catch(const std::exception &){return nullptr;}
 }
 ~Worker() {
  stop=true;wake.notify_all();
  {std::lock_guard lock(connectionMutex);if(active)fv_connection_interrupt(active);}
  if(thread.joinable())thread.join();
 }
 void run() {
  std::map<std::string,std::shared_ptr<const std::string>> geometry;
  std::unique_ptr<fv_connection,decltype(&fv_connection_close)> connection(nullptr,fv_connection_close);
  auto clearConnection=[&]{
   std::lock_guard lock(connectionMutex);active=nullptr;connection.reset();
  };
  while(!stop) {
  try {
   auto credentials=readBytes(grant,4096);
   fvp_header header;fvp_reader reader;size_t consumed=0;fvp_field token;
   need(fvp_frame_decode(credentials.data(),credentials.size(),&header,&reader,&consumed)==FVP_OK &&
    header.domain==2 && header.kind==10 && consumed==credentials.size() &&
    fvp_reader_next(&reader,&token)==FVP_OK && token.tag==1 && token.type==FVP_BYTES &&
    token.size==32,"read grant format");
   std::filesystem::create_directories(cache);
   fv_connection_config config{sizeof(config),1000,socket.c_str(),token.data};
   fv_connection *raw=nullptr;need(fv_connection_open(&config,&raw)==FV_OK,"read gateway unavailable");
   {std::lock_guard lock(connectionMutex);connection.reset(raw);active=raw;}
   auto load=[&](const fv_uuid &world,const fv_digest &sha) {
    const auto key=digest(sha);auto found=geometry.find(key);if(found!=geometry.end())return found->second;
    need(geometry.size()<32,"geometry cache limit");
    auto path=std::filesystem::path(cache)/key;uint64_t bytes=0;
    if(!std::filesystem::exists(path))
     need(fv_connection_asset(raw,&world,&sha,path.string().c_str(),&bytes)==FV_OK,"asset stream refused");
    need(fv_verify_asset_file(path.string().c_str(),&sha,&bytes)==FV_OK,"asset integrity");
    auto data=readBytes(path,16*1024*1024);
    size_t total=data.size();for(const auto &g:geometry)total+=g.second->size();
    need(total<=64*1024*1024,"geometry byte budget");
    auto value=std::make_shared<const std::string>(reinterpret_cast<const char *>(data.data()),data.size());
    geometry.emplace(key,value);return value;
   };
   while(!stop) {
    auto next=std::make_shared<Observation>();next->received=Clock::now();
    fv_session *handle=nullptr;
    need(fv_connection_snapshot(raw,&handle,&next->stamp)==FV_OK,"current state unavailable");
    std::unique_ptr<fv_session,decltype(&fv_session_close)> session(handle,fv_session_close);
    for(uint32_t i=0;i<fv_session_world_count(handle);++i) {
     World w;need(fv_session_world(handle,i,&w.info)==FV_OK,"world scope");
     for(uint32_t j=0;j<w.info.residents;++j) {
      fv_resident_info actor{};need(fv_session_resident(handle,&w.info.world,j,&actor)==FV_OK,"resident scope");
      w.residents.push_back(actor);
      fv_resident_body_info body{};
      if(fv_session_resident_body(handle,&w.info.world,j,&body)==FV_OK) {
       Shape shape;shape.hash=digest(body.view_glb_sha256);shape.glb=load(w.info.world,body.view_glb_sha256);shape.entity=identity(actor.entity);
       std::copy(std::begin(actor.position),std::end(actor.position),shape.pose.position);
       shape.resident=true;w.shapes.push_back(std::move(shape));
      }
     }
     for(uint32_t j=0;j<w.info.assets;++j) {
      fv_asset_info asset{};need(fv_session_asset(handle,&w.info.world,j,&asset)==FV_OK,"asset scope");
      Shape shape;shape.hash=digest(asset.content_sha256);shape.glb=load(w.info.world,asset.content_sha256);shape.entity=identity(asset.entity);shape.asset=identity(asset.asset);shape.version=digest(asset.version);
      need(fv_session_asset_pose(handle,&w.info.world,j,&shape.pose)==FV_OK,"source asset transform required");
      w.shapes.push_back(std::move(shape));
     }
     for(uint32_t j=0;j<w.info.events;++j) {
      fv_event_info event{};need(fv_session_event(handle,&w.info.world,j,&event)==FV_OK,"history scope");
      w.history.push_back(event);
     }
     next->worlds.push_back(std::move(w));
    }
    interaction();
    admission(next->worlds);
    next->authoredPreview=authoredPreview;
    next->storm=readStormFrame();
    if(next->storm&&!std::any_of(next->worlds.begin(),next->worlds.end(),[&](const auto&w){return identity(w.info.world)==next->storm->world&&w.info.revision==next->storm->sourceRevision;}))next->storm.reset();
    if(stop)break;
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-next->received).count();
    {std::lock_guard lock(mutex);
     if(static_cast<uint64_t>(elapsed)+next->stamp.source_age_ms<=2000){latest=next;status="Connected · current durable state";}
     else {latest.reset();status="Waiting for a current observation…";}
    }
    std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(350),[this]{return stop.load();});
   }
  }catch(const std::exception &error) {
   LL_WARNS("FinalverseNative")<<"Observation reconnecting: "<<error.what()<<LL_ENDL;
   std::lock_guard lock(mutex);latest.reset();status="Reconnecting · waiting for current world state";
  }
  clearConnection();
  std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(500),[this]{return stop.load();});
  }
 }
};
struct Batch {
 LLPointer<LLVertexBuffer> buffer;
 LLColor4 color;
};
class NativeTexture final : public LLViewerDynamicTexture {
public:
 std::shared_ptr<const Observation> observation;
 std::string selected;
 double origin[3]{0,0,0};
 F32 yaw=-.8f,distance=14.f,aspect=704.f/448.f;
 bool orbit=false,dirty=true;
 uint64_t rendered=0,triangles=0;
 double lastRenderMs=0;
 std::map<std::string,std::vector<Batch>> meshes;
 NativeTexture():LLViewerDynamicTexture(512,512,4,ORDER_MIDDLE,true){}
 bool needsRender()override {
  if(std::getenv("FINALVERSE_STORM_FRAMES"))return false;
  return observation && (orbit||dirty) &&
   std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-observation->received).count()+
   static_cast<int64_t>(observation->stamp.source_age_ms)<=2000;
 }
 const World *world()const {
  if(!observation)return nullptr;
  for(const auto &w:observation->worlds)if(identity(w.info.world)==selected)return &w;
  return nullptr;
 }
 void clear() {observation.reset();meshes.clear();selected.clear();dirty=true;}
 std::vector<Batch> load(const Shape &shape) {
  LL::GLTF::Asset asset;
  need(shape.glb->size()>=28 && shape.glb->size()<=16*1024*1024,"GLB budget");
  need(asset.loadBinary(*shape.glb,false),"native glTF loader refused asset");
  need(asset.mNodes.size()<=512 && asset.mMeshes.size()<=256,"scene graph budget");
  for(auto &scene:asset.mScenes)scene.updateTransforms(asset);
  std::vector<Batch> result;uint64_t total=0,verticesTotal=0;
  for(const auto &node:asset.mNodes) {
   if(node.mMesh<0)continue;
   need(static_cast<size_t>(node.mMesh)<asset.mMeshes.size(),"native mesh index");
   for(const auto &primitive:asset.mMeshes[node.mMesh].mPrimitives) {
    const auto count=primitive.mPositions.size(),indices=primitive.mIndexArray.size();
    if(primitive.mMode!=LL::GLTF::Primitive::Mode::TRIANGLES)continue;
    need(count>=3 && count<=65535 && indices>0 && indices%3==0 && primitive.mNormals.size()==count,"triangle attributes");
    total+=indices/3;verticesTotal+=count;
    need(total<=100000 && verticesTotal<=100000 && result.size()<256,"triangle budget");
    Batch batch;batch.color=LLColor4(.65f,.72f,.75f,1.f);
    if(primitive.mMaterial>=0 && static_cast<size_t>(primitive.mMaterial)<asset.mMaterials.size()) {
     auto color=asset.mMaterials[primitive.mMaterial].mPbrMetallicRoughness.mBaseColorFactor;
     batch.color=LLColor4(color.r,color.g,color.b,color.a);
    }
    batch.buffer=new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX|LLVertexBuffer::MAP_NORMAL|LLVertexBuffer::MAP_TEXCOORD0);
    need(batch.buffer->allocateBuffer(static_cast<S32>(count),static_cast<S32>(indices)),"vertex allocation");
    LLStrider<LLVector3> positions,normals;LLStrider<LLVector2> uv;LLStrider<U16> index;
    batch.buffer->getVertexStrider(positions);batch.buffer->getNormalStrider(normals);
    batch.buffer->getTexCoord0Strider(uv);batch.buffer->getIndexStrider(index);
    auto normalMatrix=glm::transpose(glm::inverse(node.mAssetMatrix));
    for(size_t i=0;i<count;++i) {
     auto p=primitive.mPositions[i].getF32ptr(),n=primitive.mNormals[i].getF32ptr();
     auto transformed=node.mAssetMatrix*glm::vec4(p[0],p[1],p[2],1.f);
     auto normal=normalMatrix*glm::vec4(n[0],n[1],n[2],0.f);
     LLVector3 point(transformed.x,transformed.y,transformed.z),direction(normal.x,normal.y,normal.z);
     need(point.isFinite() && direction.isFinite(),"finite geometry");
     need(std::abs(point.mV[0])<=1000000.f && std::abs(point.mV[1])<=1000000.f && std::abs(point.mV[2])<=1000000.f,"regional geometry extent");
     direction.normalize();positions[static_cast<S32>(i)]=point;normals[static_cast<S32>(i)]=direction;
     uv[static_cast<S32>(i)]=primitive.mTexCoords0.size()==count?primitive.mTexCoords0[i]:LLVector2(0,0);
    }
    for(size_t i=0;i<indices;++i) {
     need(primitive.mIndexArray[i]<count,"native triangle index");
     index[static_cast<S32>(i)]=static_cast<U16>(primitive.mIndexArray[i]);
    }
    batch.buffer->unmapBuffer();result.push_back(std::move(batch));
   }
  }
  need(!result.empty(),"renderable native geometry");
  return result;
 }
 bool render()override {
  const auto *w=world();if(!w)return false;
  const auto started=Clock::now();dirty=false;
  LLGLSUIDefault ui;LLGLDisable blend(GL_BLEND);LLGLDisable cull(GL_CULL_FACE);LLGLDepthTest depth(GL_TRUE);
  glClearColor(.035f,.065f,.085f,1.f);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  if(orbit)yaw+=.004f;
  LLVector3 target(0,0,1),camera(std::cos(yaw)*distance,std::sin(yaw)*distance,distance*.5f+1);
  LLViewerCamera::getInstance()->setOriginAndLookAt(camera,LLVector3::z_axis,target);
  LLViewerCamera::getInstance()->setAspect(aspect);
  LLViewerCamera::getInstance()->setViewNoBroadcast(LLViewerCamera::getInstance()->getDefaultFOV());
  LLViewerCamera::getInstance()->setPerspective(false,mOrigin.mX,mOrigin.mY,512,512,false,.03f,512.f);
  gObjectPreviewProgram.bind();gPipeline.enableLightsPreview();
  gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
  triangles=0;
  try {
   for(const auto &shape:w->shapes) {
    auto it=meshes.find(shape.hash);
    if(it==meshes.end()) {
     need(meshes.size()<32,"GPU geometry cache limit");
     it=meshes.emplace(shape.hash,load(shape)).first;
    }
    const auto &p=shape.pose;
    float relative[3];
    need(fv_storm_relative_position(p.position,origin,1.,relative)==FV_OK,"Storm frame precision/budget");
    gGL.pushMatrix();
    gGL.translatef(relative[0],relative[1],relative[2]);
    LLQuaternion orientation(static_cast<F32>(p.rotation[0]),static_cast<F32>(p.rotation[1]),static_cast<F32>(p.rotation[2]),static_cast<F32>(p.rotation[3]));
    LLMatrix4 rotation(orientation);gGL.multMatrix(reinterpret_cast<GLfloat *>(rotation.mMatrix));
    // glTF Y-up → authoritative region Z-up (x,-z,y).
    gGL.rotatef(90.f,1.f,0.f,0.f);gGL.scalef(static_cast<F32>(p.scale[0]),static_cast<F32>(p.scale[2]),static_cast<F32>(p.scale[1]));
    for(auto &batch:it->second) {
     gGL.diffuseColor4fv(batch.color.mV);batch.buffer->setBuffer();
     batch.buffer->drawRange(LLRender::TRIANGLES,0,batch.buffer->getNumVerts()-1,batch.buffer->getNumIndices(),0);
     triangles+=batch.buffer->getNumIndices()/3;
    }
    gGL.popMatrix();
   }
  }catch(const std::exception &error) {
   LL_WARNS("FinalverseNative")<<"Geometry admission: "<<error.what()<<LL_ENDL;
   meshes.clear();gObjectPreviewProgram.unbind();return false;
  }
  gObjectPreviewProgram.unbind();++rendered;
  lastRenderMs=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
  return true;
 }
};
}
struct LLFloaterFinalverseNative::Impl {
 std::unique_ptr<Worker> worker;
 LLPointer<NativeTexture> preview;
 std::shared_ptr<const Observation> current;
 std::shared_ptr<const InteractionStatus> action;
 std::string selected,worldList,residentList,objectList,selectedResident,selectedObject;
 std::string cameraResident,lighting="daylight",localMessage;
 uint64_t loggedCursor=UINT64_MAX;
 LLPointer<LLViewerTexture> stormImage;
 uint64_t stormFrame=0;
 std::string stormImageDefinition;
 int activeMode=0;
 bool light=false,inspector=false,initialFocus=false;
 float pitch=-.65f;
 std::array<double,3> pan{0.,0.,0.};
 bool stormConfigured=std::getenv("FINALVERSE_STORM_FRAMES")!=nullptr;
 const fv_resident_info *resident() const {
  const auto *world=preview?preview->world():nullptr;if(!world)return nullptr;
  for(const auto &r:world->residents)if(identity(r.resident)==selectedResident)return &r;
  return nullptr;
 }
 void camera() {
  const char *directory=std::getenv("FINALVERSE_STORM_FRAMES");if(!directory||!preview)return;
  namespace fs=std::filesystem;fs::path root(directory);
  try {
   if(!root.is_absolute()||!fs::is_directory(fs::symlink_status(root)))return;
   if((fs::status(root).permissions()&(fs::perms::group_all|fs::perms::others_all))!=fs::perms::none)return;
   boost::json::object control{{"schema","finalverse.presentation-control/0.1"},{"yaw",preview->yaw},{"pitch",pitch},{"distance",preview->distance},{"orbit",preview->orbit},{"pan",boost::json::array{pan[0],pan[1],pan[2]}},{"lighting",lighting}};
   if(!cameraResident.empty())control["target_resident"]=cameraResident;
   const auto raw=boost::json::serialize(control);
   {std::ofstream out(root/"camera.json.part");out<<raw;need(out.good(),"camera file write");}
   fs::permissions(root/"camera.json.part",fs::perms::owner_read|fs::perms::owner_write);
   fs::rename(root/"camera.json.part",root/"camera.json");
   {std::ofstream out(root/"camera.part");out<<preview->yaw<<" "<<pitch<<" "<<preview->distance<<" "<<(preview->orbit?1:0)<<"\n";}
   fs::permissions(root/"camera.part",fs::perms::owner_read|fs::perms::owner_write);
   fs::rename(root/"camera.part",root/"camera.txt");
  }catch(const std::exception &){}
 }
 void focus() {
  if(!preview)return;
  if(const auto *target=resident()) {
   std::copy(std::begin(target->position),std::end(target->position),preview->origin);
   cameraResident=identity(target->resident);pan={0.,0.,0.};
  }
  preview->distance=stormConfigured?12.f:8.f;preview->dirty=true;camera();
 }
 void overview() {
  if(!preview)return;cameraResident.clear();pan={0.,0.,0.};pitch=-.65f;
  preview->distance=stormConfigured?600.f:32.f;preview->yaw=-.8f;preview->orbit=false;preview->dirty=true;camera();
 }
};
LLFloaterFinalverseNative::LLFloaterFinalverseNative(const LLSD &key):LLFloater(key),mNative(std::make_unique<Impl>()){}
LLFloaterFinalverseNative::~LLFloaterFinalverseNative(){disconnect();}
bool LLFloaterFinalverseNative::postBuild() {
 for(int i=0;i<3;i++)getChild<LLButton>(i==0?"mode_explore":i==1?"mode_create":"mode_develop")->setClickedCallback([this,i](LLUICtrl *,const LLSD &){mode(i);});
 getChild<LLButton>("theme")->setClickedCallback([this](LLUICtrl *,const LLSD &){mNative->light=!mNative->light;appearance();});
 getChild<LLButton>("connect")->setClickedCallback([this](LLUICtrl *,const LLSD &){connect();});
 getChild<LLButton>("disconnect")->setClickedCallback([this](LLUICtrl *,const LLSD &){disconnect();});
 getChild<LLButton>("orbit")->setClickedCallback([this](LLUICtrl *,const LLSD &){if(mNative->preview){mNative->preview->orbit=!mNative->preview->orbit;mNative->camera();}});
 getChild<LLButton>("focus")->setClickedCallback([this](LLUICtrl *,const LLSD &){mNative->focus();});
 getChild<LLButton>("overview")->setClickedCallback([this](LLUICtrl *,const LLSD &){mNative->overview();});
 getChild<LLButton>("inspect")->setClickedCallback([this](LLUICtrl *,const LLSD &){mNative->inspector=!mNative->inspector;arrange();});
 getChild<LLButton>("move_east")->setClickedCallback([this](LLUICtrl *,const LLSD &){LLSD a;a["direction"]="EAST";submit("resident.move",a);});
 getChild<LLButton>("move_west")->setClickedCallback([this](LLUICtrl *,const LLSD &){LLSD a;a["direction"]="WEST";submit("resident.move",a);});
 getChild<LLButton>("preview_plaza")->setClickedCallback([this](LLUICtrl *,const LLSD &){LLSD a;a["request"]="Preview a small public plaza with paths, trees, seating and a sculpture.";submit("scene.preview",a);});
 getChild<LLButton>("send")->setClickedCallback([this](LLUICtrl *,const LLSD &){intent();});
 getChild<LLLineEditor>("intent")->setCommitCallback([this](LLUICtrl *,const LLSD &){intent();});
 getChild<LLButton>("approve")->setClickedCallback([this](LLUICtrl *,const LLSD &){
  if(!mNative->action||!mNative->action->requiresApproval||mNative->action->stage!="accepted"||mNative->action->resident!=mNative->selectedResident)return;
  LLSD a;a["proposal_id"]=mNative->action->proposal;submit("proposal.approve",a);
 });
 getChild<LLComboBox>("lighting")->setValue("daylight");
 getChild<LLComboBox>("lighting")->setCommitCallback([this](LLUICtrl *,const LLSD &){mNative->lighting=getChild<LLComboBox>("lighting")->getValue().asString();mNative->camera();});
 getChild<LLComboBox>("resident_select")->setCommitCallback([this](LLUICtrl *,const LLSD &){mNative->selectedResident=getChild<LLComboBox>("resident_select")->getValue().asString();mNative->localMessage="Resident selected. Info shows the authoritative identity; Focus changes only the camera.";});
 getChild<LLComboBox>("objects")->setCommitCallback([this](LLUICtrl *,const LLSD &){mNative->selectedObject=getChild<LLComboBox>("objects")->getValue().asString();});
 getChild<LLComboBox>("worlds")->setCommitCallback([this](LLUICtrl *,const LLSD &){
  mNative->selected=getChild<LLComboBox>("worlds")->getValue().asString();mNative->loggedCursor=UINT64_MAX;mNative->residentList.clear();mNative->objectList.clear();mNative->selectedResident.clear();mNative->selectedObject.clear();mNative->action.reset();
  if(mNative->preview){mNative->preview->selected=mNative->selected;mNative->overview();}
 });
 appearance();arrange();return true;
}
void LLFloaterFinalverseNative::appearance() {
 const bool light=mNative->light;
 const LLColor4 surface=light?LLColor4(.94f,.96f,.98f,1.f):LLColor4(.035f,.047f,.067f,1.f);
 const LLColor4 panel=light?LLColor4(.87f,.91f,.95f,1.f):LLColor4(.055f,.071f,.098f,1.f);
 const LLColor4 ink=light?LLColor4(.09f,.14f,.21f,1.f):LLColor4(.85f,.91f,.98f,1.f);
 setBackgroundColor(surface);setBackgroundOpaque(true);
 for(const auto *name:{"header_surface","command_surface","inspector_surface"}){auto p=getChild<LLPanel>(name);p->setBackgroundColor(panel);p->setBackgroundOpaque(true);}
 for(const auto *name:{"brand","connection","inspector_title","residents","scene","action_status","history","shortcuts"})getChild<LLTextBox>(name)->setColor(ink);
 getChild<LLButton>("theme")->setLabel(light?"Dark":"Light");
}
void LLFloaterFinalverseNative::mode(int value) {
 mNative->activeMode=llclamp(value,0,2);arrange();
}
void LLFloaterFinalverseNative::reshape(S32 width,S32 height,bool parent) {
 LLFloater::reshape(width,height,parent);if(getChildView("viewport",false))arrange();
}
void LLFloaterFinalverseNative::arrange() {
 const int w=getRect().getWidth(),h=getRect().getHeight();const bool sidebar=mNative->activeMode!=0||mNative->inspector;
 auto place=[&](const char *name,int x,int top,int width,int height){LLRect r;r.setOriginAndSize(x,h-top-height,std::max(1,width),std::max(1,height));getChildView(name)->setShape(r);};
 place("header_surface",0,22,w,70);place("brand",22,35,122,28);
 for(int i=0;i<3;i++){const auto *n=i==0?"mode_explore":i==1?"mode_create":"mode_develop";place(n,152+i*88,30,80,30);getChild<LLButton>(n)->setToggleState(mNative->activeMode==i);}
 place("worlds",w-610,30,280,30);place("theme",w-322,30,70,30);place("connect",w-244,30,108,30);place("disconnect",w-128,30,108,30);
 // Compact header keeps all modes reachable at the minimum width.
 if(w<1060)place("worlds",420,30,w-750,30);
 place("connection",22,68,w-44,18);
 const int bottom=232,viewHeight=h-100-bottom,side=sidebar?292:0;
 place("viewport",14,100,w-28-side-(sidebar?10:0),viewHeight);
 if(mNative->preview)mNative->preview->aspect=static_cast<float>(getChildView("viewport")->getRect().getWidth())/std::max(1,viewHeight);
 place("inspector_surface",w-14-292,100,292,viewHeight);
 place("inspector_title",w-14-274,116,256,24);place("objects",w-14-274,150,256,28);
 const int residentHeight=std::max(148,static_cast<int>((viewHeight-104)*.62));
 const int sceneTop=192+residentHeight+10;
 place("residents",w-14-274,192,256,residentHeight);place("scene",w-14-274,sceneTop,256,std::max(76,100+viewHeight-sceneTop-12));
 if(viewHeight<340){
  place("inspector_title",w-288,110,256,20);place("objects",w-288,134,256,24);
  place("residents",w-288,164,256,78);place("scene",w-288,248,256,std::max(32,viewHeight-152));
 }
 for(const auto *name:{"inspector_surface","inspector_title","objects","residents","scene"})getChildView(name)->setVisible(sidebar);
 getChild<LLTextBox>("inspector_title")->setText(std::string(mNative->activeMode==1?"Creation & provenance":mNative->activeMode==2?"World diagnostics":"Resident & world"));
 getChild<LLButton>("inspect")->setToggleState(sidebar);
 const int base=h-222;place("command_surface",14,base,w-28,210);
 place("resident_select",28,base+12,210,28);place("inspect",246,base+12,54,28);place("focus",308,base+12,64,28);place("overview",380,base+12,86,28);place("orbit",474,base+12,66,28);place("lighting",548,base+12,100,28);
 place("move_east",656,base+12,92,28);place("move_west",756,base+12,92,28);place("preview_plaza",w-148,base+12,120,28);
 getChildView("move_west")->setVisible(w>=1100);getChildView("preview_plaza")->setVisible(w>=1100||mNative->activeMode==1);
 if(w<1100&&mNative->activeMode==1){getChildView("move_east")->setVisible(false);place("preview_plaza",656,base+12,120,28);}else getChildView("move_east")->setVisible(w>=1000);
 place("intent",28,base+52,w-384,30);place("send",w-344,base+52,136,30);place("approve",w-200,base+52,172,30);
 place("action_status",28,base+90,w-56,88);place("history",28,base+178,w-56,18);place("shortcuts",28,h-24,w-56,18);
}
void LLFloaterFinalverseNative::submit(const std::string &operation,const LLSD &arguments) {
 const auto *world=mNative->preview?mNative->preview->world():nullptr;
 if(!world||!mNative->worker||!mNative->resident()){mNative->localMessage="Select a current connected resident first.";return;}
 if(mNative->action&&mNative->action->stage=="pending"){mNative->localMessage="A request is already pending. Wait for its owning-service result.";return;}
 const std::string id=LLUUID::generateNewID().asString();
 boost::json::object request{{"schema","finalverse.viewer-request/0.1"},{"id",id},{"world",identity(world->info.world)},{"region",identity(world->info.region)},{"resident",mNative->selectedResident},{"source_revision",std::to_string(world->info.revision)},{"operation",operation},{"arguments",LlsdToJson(arguments)}};
 if(!mNative->worker->submitInteraction(boost::json::serialize(request),id,mNative->selected,identity(world->info.region),mNative->selectedResident)){mNative->localMessage="The private interaction gateway is not configured or is busy. No action was submitted.";return;}
 mNative->localMessage.clear();
 {std::lock_guard lock(mNative->worker->mutex);mNative->action=mNative->worker->interactionStatus;}
 getChild<LLButton>("approve")->setEnabled(false);
}
void LLFloaterFinalverseNative::intent() {
 auto input=getChild<LLLineEditor>("intent");std::string text=input->getText();LLStringUtil::trim(text);if(text.empty())return;
 if(text.size()>1024){mNative->localMessage="Requests are limited to1024 UTF-8 bytes.";return;}
 std::string command=text;LLStringUtil::toLower(command);
 if(command=="sunset"||command=="show this world at sunset"||command=="daylight"){
  mNative->lighting=command=="daylight"?"daylight":"sunset";getChild<LLComboBox>("lighting")->setValue(mNative->lighting);mNative->camera();mNative->localMessage="Lighting preview changed locally. Authoritative world state is unchanged.";
 }else if(command=="focus"||command=="move the camera closer to elara") {
  if(command!="focus"){
   const auto *w=mNative->preview?mNative->preview->world():nullptr;const fv_resident_info *elara=nullptr;
   if(w)for(const auto &r:w->residents)if(std::string(r.name).starts_with("Elara "))elara=&r;
   if(!elara){mNative->localMessage="Elara is not present in this connected world.";return;}
   mNative->selectedResident=identity(elara->resident);getChild<LLComboBox>("resident_select")->setValue(mNative->selectedResident);
  }
  mNative->focus();mNative->localMessage="Camera focused on the selected persistent resident.";
 }else if(command=="cinematic"||command=="use a cinematic camera view") {if(mNative->preview){mNative->focus();mNative->preview->distance=24.f;mNative->preview->orbit=true;mNative->pitch=-.28f;mNative->camera();}mNative->localMessage="Cinematic orbit is a local presentation setting.";
 }else if(command=="move east"||command=="move west") {LLSD a;a["direction"]=command=="move east"?"EAST":"WEST";submit("resident.move",a);
 }else if(command.find("plaza")!=std::string::npos&&(command.find("preview")!=std::string::npos||command.find("create")!=std::string::npos)) {LLSD a;a["request"]=text;submit("scene.preview",a);
 }else if(command.starts_with("say ")) {LLSD a;a["text"]=text.substr(4);submit("resident.communicate",a);
 }else if(command=="interact") {if(mNative->selectedObject.empty()){mNative->localMessage="Select an object in Info before proposing interaction.";return;}LLSD a;a["target"]=mNative->selectedObject;submit("resident.interact",a);
 }else {LLSD a;a["request"]=text;submit("resident.observe",a);}
 input->setText(std::string());
}
void LLFloaterFinalverseNative::connect() {
 disconnect();const char *socket=std::getenv("FINALVERSE_NATIVE_SOCKET"),*grant=std::getenv("FINALVERSE_NATIVE_GRANT"),*cache=std::getenv("FINALVERSE_NATIVE_CACHE");
 if(!socket||!grant||!cache){getChild<LLTextBox>("connection")->setText(std::string("No Finalverse universe is configured."));return;}
 mNative->preview=new NativeTexture;mNative->worker=std::make_unique<Worker>(socket,grant,cache);arrange();
}
void LLFloaterFinalverseNative::disconnect() {
 mNative->worker.reset();mNative->current.reset();mNative->action.reset();mNative->stormImage=nullptr;mNative->stormFrame=0;mNative->stormImageDefinition.clear();mNative->initialFocus=false;
 if(mNative->preview)mNative->preview->clear();mNative->preview=nullptr;
 mNative->localMessage.clear();mNative->selected.clear();mNative->worldList.clear();mNative->residentList.clear();mNative->objectList.clear();mNative->selectedResident.clear();mNative->selectedObject.clear();mNative->cameraResident.clear();
 for(const auto *name:{"residents","scene","history"})if(getChildView(name,false))getChild<LLTextBox>(name)->setText(std::string());
 for(const auto *name:{"worlds","resident_select","objects"})if(getChildView(name,false))getChild<LLComboBox>(name)->removeall();
 if(getChildView("approve",false))getChild<LLButton>("approve")->setEnabled(false);
 if(getChildView("connection",false))getChild<LLTextBox>("connection")->setText(std::string("Disconnected · displayed state cleared"));
 if(getChildView("action_status",false))getChild<LLTextBox>("action_status")->setText(std::string("Disconnected. Reconnect to reconcile any outstanding authoritative result."));
}
void LLFloaterFinalverseNative::onOpen(const LLSD &){
 if(getParent()){
  const auto available=getParent()->getRect();
  reshape(std::min(getRect().getWidth(),available.getWidth()-16),std::min(getRect().getHeight(),available.getHeight()-16));
  setOrigin(8,8);
 }
 connect();
}
void LLFloaterFinalverseNative::onClose(bool){disconnect();}
bool LLFloaterFinalverseNative::handleKeyHere(KEY key,MASK mask) {
 if(mask==MASK_CONTROL&&key>='1'&&key<='3'){mode(key-'1');return true;}
 if(getChild<LLLineEditor>("intent")->hasFocus())return LLFloater::handleKeyHere(key,mask);
 if(mask==MASK_NONE&&mNative->preview){
  if(key=='O')mNative->preview->orbit=!mNative->preview->orbit;
  else if(key=='F'){mNative->focus();return true;}
  else if(key==KEY_HOME){mNative->overview();return true;}
  else if(key==KEY_LEFT)mNative->preview->yaw-=.12f;
  else if(key==KEY_RIGHT)mNative->preview->yaw+=.12f;
  else if(key==KEY_UP)mNative->pitch=llclamp(mNative->pitch+.08f,-1.4f,-.08f);
  else if(key==KEY_DOWN)mNative->pitch=llclamp(mNative->pitch-.08f,-1.4f,-.08f);
  else if(key=='A')mNative->pan[0]=llclamp(mNative->pan[0]-2.,-1000.,1000.);
  else if(key=='D')mNative->pan[0]=llclamp(mNative->pan[0]+2.,-1000.,1000.);
  else if(key=='W')mNative->pan[1]=llclamp(mNative->pan[1]+2.,-1000.,1000.);
  else if(key=='S')mNative->pan[1]=llclamp(mNative->pan[1]-2.,-1000.,1000.);
  else return LLFloater::handleKeyHere(key,mask);
  mNative->preview->dirty=true;mNative->camera();return true;
 }
 return LLFloater::handleKeyHere(key,mask);
}
bool LLFloaterFinalverseNative::handleScrollWheel(S32 x,S32 y,S32 clicks) {
 if(mNative->preview&&getChildView("viewport")->getRect().pointInRect(x,y)){
  mNative->preview->distance=llclamp(mNative->preview->distance*std::pow(1.12f,static_cast<F32>(clicks)),2.f,mNative->stormConfigured?2000.f:120.f);mNative->preview->dirty=true;mNative->camera();return true;
 }return LLFloater::handleScrollWheel(x,y,clicks);
}
void LLFloaterFinalverseNative::draw() {
 if(mNative->worker) {
  std::shared_ptr<const Observation> data;std::string status;
  {std::lock_guard lock(mNative->worker->mutex);data=mNative->worker->latest;status=mNative->worker->status;}
  if(data&&data->worlds.empty())data.reset();
  if(data && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-data->received).count()+
    static_cast<int64_t>(data->stamp.source_age_ms)>2000)data.reset();
  getChild<LLTextBox>("connection")->setText(std::string(data?"Live world · confirmed state":"No current observation · displayed state cleared"));
  if(data&&data->authoredPreview)getChild<LLTextBox>("connection")->setText(std::string("AUTHORED PREVIEW · not published · no creation effects committed · residents remain authoritative"));
  std::shared_ptr<const InteractionStatus> action;
  {std::lock_guard lock(mNative->worker->mutex);action=mNative->worker->interactionStatus;}
  const bool matchingAction=data&&action&&std::any_of(data->worlds.begin(),data->worlds.end(),[&](const auto &w){return identity(w.info.world)==mNative->selected&&action->world==mNative->selected&&action->region==identity(w.info.region);});
  mNative->action=matchingAction?action:nullptr;
  if(mNative->action&&(mNative->localMessage.empty()||mNative->action->requiresApproval)) {
   std::string label=mNative->action->stage,detail=mNative->action->detail;
   if(label=="reconciled"&&!mNative->action->command.empty()){
    bool displayed=false;
    try{const auto revision=unsignedJson(boost::json::value(mNative->action->revision));displayed=data&&data->storm&&data->storm->world==mNative->selected&&data->storm->sourceRevision>=revision&&!data->storm->interpolating;}catch(const std::exception &){}
    if(!displayed){label="committed";detail="Authority committed. Waiting for matching settled native presentation.";}
   }
   LLStringUtil::toUpper(label);
   getChild<LLTextBox>("action_status")->setText(label+" · "+detail+(mNative->action->command.empty()?"":"\nWorldLine command "+mNative->action->command));
  }else getChild<LLTextBox>("action_status")->setText(mNative->localMessage.empty()?"Presentation controls are local. Persistent changes require a concrete reviewed proposal.":mNative->localMessage);
  getChild<LLButton>("approve")->setEnabled(data&&mNative->action&&mNative->action->stage=="accepted"&&mNative->action->requiresApproval&&mNative->action->resident==mNative->selectedResident);
  if(data!=mNative->current) {
   mNative->current=data;
   if(!data){if(mNative->preview)mNative->preview->clear();getChild<LLTextBox>("residents")->setText(std::string(""));getChild<LLTextBox>("scene")->setText(std::string(""));getChild<LLTextBox>("history")->setText(std::string(""));}
   else {
    auto combo=getChild<LLComboBox>("worlds");
    std::string list;
    for(const auto &world:data->worlds)list+=identity(world.info.world)+":"+world.info.name+"\n";
    if(list!=mNative->worldList) {
     combo->removeall();
     for(const auto &world:data->worlds)combo->add(std::string(world.info.name),identity(world.info.world));
     mNative->worldList=list;
    }
    if(mNative->selected.empty())mNative->selected=identity(data->worlds.front().info.world);
    if(combo->getValue().asString()!=mNative->selected)combo->setValue(mNative->selected);
    auto &preview=mNative->preview;preview->observation=data;preview->selected=mNative->selected;preview->dirty=true;
    if(!mNative->initialFocus){
     mNative->overview();
     if(mNative->stormConfigured&&mNative->activeMode==0){mNative->preview->distance=200.f;mNative->pitch=-.35f;mNative->camera();}
     mNative->initialFocus=true;
    }
   }
  }
  if(data && mNative->preview) {
   const auto *world=mNative->preview->world();
   if(world) {
    std::string residentList,objectList;
    for(const auto &actor:world->residents)residentList+=identity(actor.resident)+actor.name;
    if(residentList!=mNative->residentList){
     auto combo=getChild<LLComboBox>("resident_select");combo->removeall();
     const bool retained=std::any_of(world->residents.begin(),world->residents.end(),[&](const auto &r){return identity(r.resident)==mNative->selectedResident;});
     if(!retained)mNative->selectedResident.clear();
     for(const auto &actor:world->residents){combo->add(std::string(actor.name),identity(actor.resident));if(!retained&&(mNative->selectedResident.empty()||std::string(actor.name).starts_with("Elara ")))mNative->selectedResident=identity(actor.resident);}
     combo->setValue(mNative->selectedResident);mNative->residentList=residentList;
    }
    for(const auto &shape:world->shapes)if(!shape.resident)objectList+=shape.entity+shape.asset;
    if(objectList!=mNative->objectList){
     auto combo=getChild<LLComboBox>("objects");combo->removeall();
     if(!std::any_of(world->shapes.begin(),world->shapes.end(),[&](const auto &shape){return !shape.resident&&shape.entity==mNative->selectedObject;}))mNative->selectedObject.clear();
     for(const auto &shape:world->shapes)if(!shape.resident){combo->add("Published object · "+shape.entity.substr(0,8),shape.entity);if(mNative->selectedObject.empty())mNative->selectedObject=shape.entity;}
     if(mNative->selectedObject.empty())combo->add("No published objects","");
     combo->setValue(mNative->selectedObject);mNative->objectList=objectList;
    }
    const bool compact=getChildView("viewport")->getRect().getHeight()<340;
    std::ostringstream residents;
    if(const auto *actor=mNative->resident()){
     const auto residentId=identity(actor->resident),entityId=identity(actor->entity);
     getChild<LLTextBox>("residents")->setToolTip("Resident "+residentId+"\nEntity "+entityId);
     if(compact)residents<<actor->name<<" · "+residentId.substr(0,8)<<"\nBody "+entityId.substr(0,8)<<"\n";
     else residents<<"Persistent identity\n"<<actor->name<<"\n"<<residentId<<"\nEntity "<<entityId<<"\n";
     residents<<std::fixed<<std::setprecision(3)<<(compact?"Region ":"Local Z-up (m)\n")<<actor->position[0]<<", "<<actor->position[1]<<", "<<actor->position[2];
     if(data->storm){auto registered=data->storm->registeredResidents.find(residentId);if(registered!=data->storm->registeredResidents.end())residents<<(compact?"\nMap ":"\nDisplayed E,N,U (m)\n")<<registered->second[0]<<", "<<registered->second[1]<<", "<<registered->second[2];}
    }else residents<<"Select a resident to inspect.";
    getChild<LLTextBox>("residents")->setText(residents.str());
    std::ostringstream scene;scene<<"Committed revision "<<world->info.revision<<"\nWorld "<<identity(world->info.world).substr(0,8)<<" · region "<<identity(world->info.region).substr(0,8);
    getChild<LLTextBox>("scene")->setToolTip("World "+identity(world->info.world)+"\nRegion "+identity(world->info.region)+"\nObject "+mNative->selectedObject+"\nRegion axes: x,y,z-up metres; map axes: east,grid-north,up metres.");
    if(!compact){
     scene<<"\n"<<world->info.frame;
     if(mNative->activeMode==1)scene<<"\nPlaza details are authored previews. Publication requires owner review.";
     if(mNative->activeMode==2){scene<<"\n"<<world->info.assets<<" published assets · cursor "<<data->stamp.cursor;scene<<"\nObject "<<mNative->selectedObject.substr(0,8);}
    }
    getChild<LLTextBox>("scene")->setText(scene.str());
    if(!world->history.empty()){
     const auto &event=world->history.back();getChild<LLTextBox>("history")->setText("WorldLine #"+std::to_string(event.sequence)+" · "+event.kind+" · "+std::to_string(event.command_count)+" causal command(s) · "+digest(event.source_sha256).substr(0,20));
    }
    if(mNative->loggedCursor!=data->stamp.cursor) {
     LL_INFOS("FinalverseNative")<<"world="<<identity(world->info.world)<<" revision="<<world->info.revision
      <<" cursor="<<data->stamp.cursor<<" residents="<<world->residents.size()<<" assets="<<world->info.assets
      <<" native_frames="<<mNative->preview->rendered<<" triangles="<<mNative->preview->triangles
      <<" last_render_ms="<<mNative->preview->lastRenderMs<<LL_ENDL;
     mNative->loggedCursor=data->stamp.cursor;
    }
   }
  }
  if(mNative->stormConfigured){
   const auto frame=data?data->storm:nullptr;
   if(frame&&frame->world==mNative->selected&&Clock::now()-frame->received<std::chrono::seconds(2)){
    if(frame->number!=mNative->stormFrame||frame->definition!=mNative->stormImageDefinition){
     LLPointer<LLImageRaw> raw=new LLImageRaw(2048,1024,4);
     {LLImageDataLock image_lock(raw);if(!raw->isBufferInvalid()){
      std::memset(raw->getData(),0,2048*1024*4);
      for(int y=0;y<720;y++)std::memcpy(raw->getData()+y*2048*4,frame->rgba.data()+(719-y)*1280*4,1280*4);
     }}
     if(!raw->isBufferInvalid()){
      if(mNative->stormImage.isNull())mNative->stormImage=LLViewerTextureManager::getLocalTexture(raw.get(),false);
      else mNative->stormImage->setSubImage(raw.get(),0,0,2048,1024);
      mNative->stormFrame=frame->number;mNative->stormImageDefinition=frame->definition;
      LL_INFOS("FinalverseNative")<<"Storm Metal displayed world="<<frame->world<<" definition="<<frame->definition<<" frame="<<frame->number<<" triangles="<<frame->triangles<<LL_ENDL;
     }
    }
    if(mNative->activeMode==2)getChild<LLTextBox>("scene")->setText(getChild<LLTextBox>("scene")->getText()+"\nStorm Metal · "+std::to_string(frame->triangles)+" triangles · frame "+std::to_string(frame->number));
   }else {mNative->stormImage=nullptr;mNative->stormFrame=0;mNative->stormImageDefinition.clear();getChild<LLTextBox>("connection")->setText(std::string("Waiting for a current, world-scoped Storm frame · viewport cleared"));}
  }
 }
 LLFloater::draw();
 if(mNative->stormConfigured){
  if(mNative->stormImage.notNull()){
   auto r=getChildView("viewport")->getRect();LLGLSUIDefault gls_ui;gGL.color4f(1,1,1,1);
   // Fill the immersive viewport by cropping, never by stretching geography.
   const float aspect=static_cast<float>(r.getWidth())/std::max(1,r.getHeight()),imageAspect=1280.f/720.f;
   float left=0.f,right=1280.f/2048.f,bottom=0.f,top=720.f/1024.f;
   if(mNative->activeMode==2){
    if(aspect>imageAspect){const int width=static_cast<int>(r.getHeight()*imageAspect);r.mLeft+=(r.getWidth()-width)/2;r.mRight=r.mLeft+width;}
    else {const int height=static_cast<int>(r.getWidth()/imageAspect);r.mBottom+=(r.getHeight()-height)/2;r.mTop=r.mBottom+height;}
   }else if(aspect>imageAspect){const float span=(top-bottom)*imageAspect/aspect;bottom=(top-span)*.5f;top=bottom+span;}
   else {const float span=(right-left)*aspect/imageAspect;left=(right-span)*.5f;right=left+span;}
   gl_draw_scaled_image(r.mLeft,r.mBottom,r.getWidth(),r.getHeight(),mNative->stormImage,LLColor4::white,LLRectf(left,top,right,bottom));
  }return;
 }
 if(mNative->current && mNative->preview && mNative->preview->rendered) {
  auto r=getChildView("viewport")->getRect();
  gUIProgram.bind();gGL.getTexUnit(0)->bind(mNative->preview);gGL.color4f(1,1,1,1);
  gGL.begin(LLRender::TRIANGLES);
  gGL.texCoord2f(0,1);gGL.vertex2i(r.mLeft,r.mTop);gGL.texCoord2f(0,0);gGL.vertex2i(r.mLeft,r.mBottom);gGL.texCoord2f(1,0);gGL.vertex2i(r.mRight,r.mBottom);
  gGL.texCoord2f(1,0);gGL.vertex2i(r.mRight,r.mBottom);gGL.texCoord2f(1,1);gGL.vertex2i(r.mRight,r.mTop);gGL.texCoord2f(0,1);gGL.vertex2i(r.mLeft,r.mTop);
  gGL.end();gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
 }
}
void llfinalverseNativeMaybeOpen() {
 static bool opened=false;
 if(!opened && std::getenv("FINALVERSE_NATIVE_SOCKET") && gObjectPreviewProgram.mProgramObject) {
  opened=true;LLFloaterReg::showInstance("finalverse_native");
 }
}
