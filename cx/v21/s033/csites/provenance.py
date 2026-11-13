import hashlib,json,pathlib as P,subprocess as S
j=P.Path(__file__).parent;r=P.Path('/root/worktree/zlink-3867h')
def sha(p):
 h=hashlib.sha256()
 with P.Path(p).open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 return h.hexdigest()
d={'head':S.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'branch':S.check_output(['git','branch','--show-current'],cwd=r,text=True).strip(),'diff_sha256':hashlib.sha256(S.check_output(['git','diff','--binary'],cwd=r)).hexdigest(),'binaries':{}}
assert d['head']=='0cd1eac0c5fb367d0373f882094363a556b47478'
assert d['branch']=='perf/3867-cpp-request-sites'
changed=S.check_output(['git','diff','--name-only'],cwd=r,text=True).splitlines();assert all(x.startswith('framework/languages/cpp/') for x in changed);d['changed_files']=changed
for stage in ('before','after-final','before-perf','after-final-perf'):
 d['binaries'][stage]={name:sha(j/stage/name) for name in ('bench_cpp_client','bench_cpp_framework_server','bench_cpp_zlink_server','libchand.so')}
assert len({v['bench_cpp_zlink_server'] for v in d['binaries'].values()})==1
assert len({v['libchand.so'] for v in d['binaries'].values()}|{sha('/root/jobs/fx/cx/v21/s033/csite2/libchand.so')})==1
for name in ('libzlink_framework.a','libzlink_framework_provider_abstractions.a'):
 assert sha(j/'after-final-perf'/name)==sha(r/'framework/languages/cpp/build/linux-ninja-release'/name),name
(j/'provenance.json').write_text(json.dumps(d,indent=2));print('PASS baseline HEAD/branch, write scope, unchanged binding and interceptor, final performance archives match build')
