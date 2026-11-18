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
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
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
struct Shape {
 std::string hash;
 std::shared_ptr<const std::string> glb;
 fv_pose pose{{0,0,0},{0,0,0,1},{1,1,1}};
 bool resident=false;
};
struct World {
 fv_world_info info{};
 std::vector<fv_resident_info> residents;
 std::vector<Shape> shapes;
 std::vector<fv_event_info> history;
};
struct Observation {
 fv_stream_stamp stamp{};
 std::vector<World> worlds;
 Clock::time_point received;
};
struct Worker {
 std::atomic<bool> stop{false};
 std::mutex mutex,connectionMutex;
 std::condition_variable wake;
 std::shared_ptr<const Observation> latest;
 fv_connection *active=nullptr;
 std::thread thread;
 std::string socket,grant,cache,status="Connecting…";
 Worker(std::string a,std::string b,std::string c):socket(std::move(a)),grant(std::move(b)),cache(std::move(c)) {
  thread=std::thread([this]{run();});
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
       Shape shape;shape.hash=digest(body.view_glb_sha256);shape.glb=load(w.info.world,body.view_glb_sha256);
       std::copy(std::begin(actor.position),std::end(actor.position),shape.pose.position);
       shape.resident=true;w.shapes.push_back(std::move(shape));
      }
     }
     for(uint32_t j=0;j<w.info.assets;++j) {
      fv_asset_info asset{};need(fv_session_asset(handle,&w.info.world,j,&asset)==FV_OK,"asset scope");
      Shape shape;shape.hash=digest(asset.content_sha256);shape.glb=load(w.info.world,asset.content_sha256);
      need(fv_session_asset_pose(handle,&w.info.world,j,&shape.pose)==FV_OK,"source asset transform required");
      w.shapes.push_back(std::move(shape));
     }
     for(uint32_t j=0;j<w.info.events;++j) {
      fv_event_info event{};need(fv_session_event(handle,&w.info.world,j,&event)==FV_OK,"history scope");
      w.history.push_back(event);
     }
     next->worlds.push_back(std::move(w));
    }
    if(stop)break;
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-next->received).count();
    {std::lock_guard lock(mutex);
     if(static_cast<uint64_t>(elapsed)+next->stamp.source_age_ms<=2000){latest=next;status="Connected · current durable state";}
     else {latest.reset();status="Waiting for a current observation…";}
    }
    std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(350),[this]{return stop.load();});
   }
  }catch(const std::exception &error) {
   LL_WARNS("FinalverseNative")<<"Observation stopped: "<<error.what()<<LL_ENDL;
   std::lock_guard lock(mutex);latest.reset();status="Disconnected · no current world state";
  }
  clearConnection();
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
 F32 yaw=-.8f,distance=14.f;
 bool orbit=false,dirty=true;
 uint64_t rendered=0,triangles=0;
 double lastRenderMs=0;
 std::map<std::string,std::vector<Batch>> meshes;
 NativeTexture():LLViewerDynamicTexture(512,512,4,ORDER_MIDDLE,true){}
 bool needsRender()override {
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
  LLViewerCamera::getInstance()->setAspect(704.f/448.f);
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
 std::string selected,worldList;
 uint64_t loggedCursor=UINT64_MAX;
 void focus() {
  if(!preview)return;
  const auto *w=preview->world();if(!w)return;
  const fv_resident_info *target=nullptr;
  for(const auto &r:w->residents)if(!target || std::string(r.name).starts_with("Elara "))target=&r;
  if(target)std::copy(std::begin(target->position),std::end(target->position),preview->origin);
  else if(!w->shapes.empty())std::copy(std::begin(w->shapes.front().pose.position),std::end(w->shapes.front().pose.position),preview->origin);
  preview->distance=std::any_of(w->shapes.begin(),w->shapes.end(),[](const auto &shape){return shape.resident;})?8.f:32.f;preview->dirty=true;
 }
};
LLFloaterFinalverseNative::LLFloaterFinalverseNative(const LLSD &key):LLFloater(key),mNative(std::make_unique<Impl>()){}
LLFloaterFinalverseNative::~LLFloaterFinalverseNative(){disconnect();}
bool LLFloaterFinalverseNative::postBuild() {
 getChild<LLButton>("connect")->setClickedCallback([this](LLUICtrl *,const LLSD &){connect();});
 getChild<LLButton>("disconnect")->setClickedCallback([this](LLUICtrl *,const LLSD &){disconnect();});
 getChild<LLButton>("orbit")->setClickedCallback([this](LLUICtrl *,const LLSD &){if(mNative->preview)mNative->preview->orbit=!mNative->preview->orbit;});
 getChild<LLButton>("focus")->setClickedCallback([this](LLUICtrl *,const LLSD &){mNative->focus();});
 getChild<LLComboBox>("worlds")->setCommitCallback([this](LLUICtrl *,const LLSD &){
  mNative->selected=getChild<LLComboBox>("worlds")->getValue().asString();
  mNative->loggedCursor=UINT64_MAX;
  if(mNative->preview){mNative->preview->selected=mNative->selected;mNative->focus();}
 });
 return true;
}
void LLFloaterFinalverseNative::connect() {
 disconnect();
 const char *socket=std::getenv("FINALVERSE_NATIVE_SOCKET"),*grant=std::getenv("FINALVERSE_NATIVE_GRANT"),*cache=std::getenv("FINALVERSE_NATIVE_CACHE");
 if(!socket||!grant||!cache){getChild<LLTextBox>("connection")->setText(std::string("No Finalverse universe is configured."));return;}
 mNative->preview=new NativeTexture;
 mNative->worker=std::make_unique<Worker>(socket,grant,cache);
}
void LLFloaterFinalverseNative::disconnect() {
 mNative->worker.reset();mNative->current.reset();
 if(mNative->preview)mNative->preview->clear();
 mNative->preview=nullptr;mNative->selected.clear();mNative->worldList.clear();
 for(const auto *name:{"residents","scene","history"})
  if(getChildView(name,false))getChild<LLTextBox>(name)->setText(std::string(""));
 if(getChildView("worlds",false))getChild<LLComboBox>("worlds")->removeall();
 if(getChildView("connection",false))getChild<LLTextBox>("connection")->setText(std::string("Disconnected · world state cleared"));
}
void LLFloaterFinalverseNative::onOpen(const LLSD &){connect();}
void LLFloaterFinalverseNative::onClose(bool){disconnect();}
bool LLFloaterFinalverseNative::handleScrollWheel(S32 x,S32 y,S32 clicks) {
 if(mNative->preview && getChildView("viewport")->getRect().pointInRect(x,y)) {
  mNative->preview->distance=llclamp(mNative->preview->distance*std::pow(1.12f,static_cast<F32>(clicks)),2.f,120.f);
  mNative->preview->dirty=true;return true;
 }
 return LLFloater::handleScrollWheel(x,y,clicks);
}
void LLFloaterFinalverseNative::draw() {
 if(mNative->worker) {
  std::shared_ptr<const Observation> data;std::string status;
  {std::lock_guard lock(mNative->worker->mutex);data=mNative->worker->latest;status=mNative->worker->status;}
  if(data && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-data->received).count()+
    static_cast<int64_t>(data->stamp.source_age_ms)>2000)data.reset();
  getChild<LLTextBox>("connection")->setText(data?status:"No current observation · displayed state cleared");
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
    if(preview->rendered==0)mNative->focus();
   }
  }
  if(data && mNative->preview) {
   const auto *world=mNative->preview->world();
   if(world) {
    std::ostringstream residents;residents<<"Persistent residents\n\n";
    for(const auto &actor:world->residents)residents<<actor.name<<"\n";
    getChild<LLTextBox>("residents")->setText(residents.str());
    std::ostringstream scene;scene<<"World revision "<<world->info.revision<<"\n"<<world->info.assets<<" published assets\n"
     <<mNative->preview->triangles<<" submitted triangles\n"<<world->info.frame;
    getChild<LLTextBox>("scene")->setText(scene.str());
    if(!world->history.empty()) {
     const auto &event=world->history.back();
     getChild<LLTextBox>("history")->setText("WorldLine "+std::to_string(event.sequence)+" · "+event.kind+" · "+
      std::to_string(event.command_count)+" causal command(s)\n"+digest(event.source_sha256));
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
 }
 LLFloater::draw();
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
