#!/usr/bin/env python3
"""Actual Metal adapter fixture. Synthetic presentation poses, no world writer.

Run separately with explicit accepted source FWD/CAS and media paths; this is
not a substitute for the parent authority→FVP→Viewer synchronization scenario.
"""
import argparse,hashlib,json,os,shutil,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for x in ['renderer','fwd','cas','media','out']:p.add_argument('--'+x,type=Path,required=True)
a=p.parse_args();os.umask(0o077);a.out.mkdir(parents=True,exist_ok=False)
fwd=json.loads(a.fwd.read_bytes());definition=hashlib.sha256(a.fwd.read_bytes()).hexdigest();assets={x['id']:x for x in fwd['assets']}
bindings=[]
for b in fwd.get('simulation_bindings',[]):
 v=assets[b['asset_id']]
 if any(s.startswith('resident-') for s in v['source_ids']):bindings.append((b,v))
assert bindings,'requires composed real resident assets'
# Fixture identities are taken from the real composition receipt, without
# generating replacements. Only desired presentation positions are synthetic.
composition=json.loads((a.fwd.parent/'composition.json').read_bytes());ids={r['entity']:r['id'] for r in composition['residents']}
residents=[{'resident':ids[b['entity_id']],'entity':b['entity_id'],'body_sha256':v['sha256'],'position':v['transform']['translation'],'source_position_region_m':[0,0,0],'motion':'stationary'} for b,v in bindings]
elara=next(r for r in residents if r['resident']=='904d952a-aca7-51da-aa01-20aab7dd9893');focus=list(elara['position'])
frames=a.out/'frames';log=(a.out/'renderer.log').open('w')
proc=subprocess.Popen([str(a.renderer),str(a.fwd),str(a.cas),str(a.media),str(frames),'1200','hidden',*map(str,focus),'0','b96ee246-06f6-496f-ad70-4ae73fb711dd'],stdout=log,stderr=subprocess.STDOUT,cwd=a.out)
def atomic(name,d):
 part=frames/(name+'.part');part.write_text(json.dumps(d,separators=(',',':')));part.replace(frames/name)
def current():
 try:
  meta=json.loads((frames/'frame.json').read_bytes());image=(frames/'frame.ppm').read_bytes()
  return meta if hashlib.sha256(image).hexdigest()==meta['pixels_sha256'] else None
 except (FileNotFoundError,json.JSONDecodeError):return None
def wait(test,seconds=8):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if proc.poll() is not None:raise RuntimeError('renderer stopped: '+(a.out/'renderer.log').read_text()[-1000:])
  m=current()
  if m and test(m):return m
  time.sleep(.02)
 raise RuntimeError('timed out waiting for pixel state')
def capture(label,m):
 (a.out/(label+'.json')).write_text(json.dumps(m,indent=2));shutil.copyfile(frames/'frame.ppm',a.out/(label+'.ppm'))
def update(cursor,rev,entries):
 return {'schema':'finalverse.scene-update/0.1','world':fwd['world']['id'],'region':'b96ee246-06f6-496f-ad70-4ae73fb711dd','epoch':'0123456789abcdef0123456789abcdef','cursor':cursor,'revision':rev,'previous_revision':rev-1 if rev>1 else None,'full_snapshot':True,'definition_sha256':definition,'residents':entries,'assets':[],'event_refs':[]}
try:
 wait(lambda m:True)
 atomic('camera.json',{'schema':'finalverse.presentation-control/0.1','yaw':.4,'pitch':-.4,'distance':16,'orbit':False,'pan':[0,0,0],'lighting':'daylight'})
 atomic('scene-update.json',update(1,1,residents));wait(lambda m:m['source_revision']==1 and len(m['registered_residents'])==len(residents));time.sleep(.7)
 before=wait(lambda m:m['source_revision']==1);capture('before',before)
 elara['position']=list(elara['position']);elara['position'][0]+=3;elara['motion']='committed'
 atomic('scene-update.json',update(2,2,residents));after=wait(lambda m:m['source_revision']==2 and not m['interpolation_active']);capture('after',after)
 assert before['pixels_sha256']!=after['pixels_sha256'],'root transform did not change actual pixels'
 x=next(r for r in after['registered_residents'] if r['resident']==elara['resident']);assert abs(x['position'][0]-elara['position'][0])<.001
 # Membership removal/recreation uses existing admitted body meshes and handles.
 atomic('scene-update.json',update(3,3,[r for r in residents if r is not elara]));removed=wait(lambda m:m['source_revision']==3);capture('removed',removed)
 assert len(removed['registered_residents'])==len(residents)-1 and removed['objects']<after['objects']
 atomic('scene-update.json',update(4,4,residents));restored=wait(lambda m:m['source_revision']==4);assert restored['objects']==after['objects']
 # Unknown body content cannot turn a stream reference into renderer geometry.
 bad=json.loads(json.dumps(residents));bad[0]['body_sha256']='0'*64
 atomic('scene-update.json',update(5,5,bad));refused=wait(lambda m:m['synchronization']=='refused');assert refused['source_revision']==4
 # Explicit full snapshot recovers after a cursor/revision gap.
 atomic('scene-update.json',update(8,8,residents));recovered=wait(lambda m:m['source_revision']==8);assert recovered['synchronization']=='snapshot-resynchronized'
 # Lighting is presentation only and does not modify source revision or poses.
 atomic('camera.json',{'schema':'finalverse.presentation-control/0.1','yaw':.4,'pitch':-.4,'distance':16,'orbit':False,'pan':[0,0,0],'lighting':'sunset'})
 sunset=wait(lambda m:m['lighting']=='sunset');capture('sunset',sunset);assert sunset['source_revision']==8 and sunset['pixels_sha256']!=restored['pixels_sha256']
 # Camera pan is local presentation state; no scene event is constructed.
 atomic('camera.json',{'schema':'finalverse.presentation-control/0.1','yaw':.6,'pitch':-.5,'distance':22,'orbit':False,'pan':[2,0,0],'lighting':'sunset','target_resident':elara['resident']})
 camera=wait(lambda m:m['lighting']=='sunset' and m['focus_origin'][0]>focus[0]+3);capture('camera',camera)
 result={'schema':'finalverse.omega-xv-render-fixture/1','synthetic_presentation_poses':True,'world_commands':0,'actual_metal_pixels':True,'dynamic_root_pixel_change':True,'removed_and_restored_known_body':True,'unknown_body_refused':True,'full_snapshot_gap_recovery':True,'lighting_changes_pixels':True,'target_camera_pan':True,'renderer_restart_count':0,'source_definition_sha256':definition,'before_pixels_sha256':before['pixels_sha256'],'after_pixels_sha256':after['pixels_sha256'],'final_source_revision':camera['source_revision'],'resident_count':len(residents),'elapsed_limit_seconds':20}
 (a.out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=8)
 log.close()
