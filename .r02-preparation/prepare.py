"""Prepare authenticated source blobs only. Never write a feature ref or commit."""
import os,sys,json,base64,zlib,hashlib,subprocess,tarfile,io,urllib.request
from pathlib import Path
ROOT=Path.cwd();OUT=ROOT/'prepared';OUT.mkdir(exist_ok=True)
BRANCH='codex/r02-preparation-20260926-d248'
EXPECTED={'hybridclr':'7c844a865bf48e70fa01036e620e37b5c6ad6ce8ec665e745249130b79e93210','hybridclr_unity':'8974e8b5325cccd81936616d868d157bbe5d0057e07469ea04ba8e07f894c988','il2cpp_plus':'9e11728e866e96ac78b4962e3269740b0525a5dba547a3ca1f0ae84e24176526','hybridclr_demo':'1b104a2be3ace7f4feca6e95695f49940dc8e810acb9486d30a37cc0baa17a61'}
OWNER=os.environ['GITHUB_REPOSITORY'];NAME=OWNER.split('/')[-1]
assert OWNER=='night-outlook/'+NAME and NAME in EXPECTED
PINS={'hybridclr_demo':'a8d2c02c636ee9276d6cf12a54ee3e0626a5c49e','hybridclr':'1d2df7c36a3f9eb99ca8242f6c2bd4a5e054f0ad','hybridclr_unity':'0ea633a2c5b936b5af69d944593c55bd2783fca9','il2cpp_plus':'6be7f38bec2fa4677d24efc1a4a1294240789933'}
def git(repo,*args):return subprocess.check_output(['git','-C',str(repo),*args])
def blob(raw):return hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()
def delta(repo):
 if repo==NAME:raw=(ROOT/'.r02-preparation/source-delta.b64').read_bytes()
 else:raw=urllib.request.urlopen('https://raw.githubusercontent.com/night-outlook/'+repo+'/'+BRANCH+'/.r02-preparation/source-delta.b64',timeout=90).read()
 assert hashlib.sha256(raw).hexdigest()==EXPECTED[repo],('delta transport mismatch',repo)
 value=json.loads(zlib.decompress(base64.b64decode(raw,validate=False)))
 assert value['kind']=='R02ReviewedSourceDelta' and value['basePins']==PINS
 return value
maps={}
for repo in (list(PINS) if NAME=='hybridclr_demo' else [NAME]):
 bare=OUT/('git-'+repo);bare.mkdir();subprocess.run(['git','init','--quiet',str(bare)],check=True)
 git(bare,'remote','add','origin','https://github.com/night-outlook/'+repo+'.git')
 git(bare,'fetch','--quiet','--depth=1','--filter=blob:none','origin',PINS[repo])
 assert git(bare,'rev-parse','FETCH_HEAD').decode().strip()==PINS[repo]
 dest=OUT/'work'/repo;dest.mkdir(parents=True)
 if NAME=='hybridclr_demo':
  paths=['Tools','Assets/AssemblyShadowDemo','ProjectSettings','Packages'] if repo=='hybridclr_demo' else []
  raw=git(bare,'archive','--format=tar',PINS[repo],*paths)
  with tarfile.open(fileobj=io.BytesIO(raw),mode='r:') as t:
   for m in t.getmembers():
    assert not Path(m.name).is_absolute() and '..' not in Path(m.name).parts and (m.isfile() or m.isdir())
    if m.isfile():
     p=dest/m.name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(t.extractfile(m).read())
 rows=[]
 for entry in delta(repo)['files']:
  assert entry['repository']==repo
  path=entry['path'];assert not Path(path).is_absolute() and '..' not in Path(path).parts
  original=entry.get('template') or path
  raw=git(bare,'show',PINS[repo]+':'+original) if entry.get('oldBlob') or entry.get('template') else b''
  expected=entry.get('templateBlob') or entry.get('oldBlob')
  if expected:assert blob(raw)==expected,(repo,path,'old source mismatch')
  lines=raw.decode().splitlines(keepends=True)
  for start,end,replacement in reversed(entry['edits']):
   assert 0<=start<=end<=len(lines);lines[start:end]=replacement
  changed=''.join(lines).encode()
  assert len(changed)==entry['bytes'] and blob(changed)==entry['newBlob'] and hashlib.sha256(changed).hexdigest()==entry['newSha256'],(repo,path,'new source mismatch')
  target=dest/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(changed)
  if repo==NAME:
   request=urllib.request.Request('https://api.github.com/repos/'+OWNER+'/git/blobs',data=json.dumps({'content':base64.b64encode(changed).decode(),'encoding':'base64'}).encode(),method='POST',headers={'Authorization':'Bearer '+os.environ['PREPARE_TOKEN'],'Accept':'application/vnd.github+json','Content-Type':'application/json'})
   result=json.load(urllib.request.urlopen(request,timeout=90));assert result['sha']==entry['newBlob']
  rows.append({k:entry[k] for k in ('path','oldBlob','newBlob','newSha256','bytes')})
 maps[repo]=rows
(OUT/'source-map.json').write_text(json.dumps({'kind':'R02PreparedSourceBlobs','featureRefsModified':False,'workflowCommit':os.environ['GITHUB_SHA'],'basePins':PINS,'files':maps},indent=2)+'\n')
if NAME=='hybridclr_demo':
 demo=OUT/'work/hybridclr_demo'
 proc=subprocess.run([sys.executable,'-B',str(demo/'Tools/AssemblyShadow/r02_primary.py'),'--demo-root',str(demo),'--output',str(OUT/'primary'),'--managed'],cwd=demo)
 sys.exit(proc.returncode)
