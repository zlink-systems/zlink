import os,subprocess,pathlib,json,hashlib,resource,datetime,re
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path('/root/jobs/fx/cx/v21/s033/c4dec');wt=pathlib.Path('/root/worktree/zlink-v21-s033/c4dec');build=wt/'build-c4dec'
names=['test_cpp_framework_typed_json','test_cpp_framework_typed_json_allocations','test_cpp_framework_typed_json_noexception','test_cpp_framework_typed_json_connector_core','test_cpp_framework_typed_json_connector_extension','test_cpp_framework_typed_json_message_pack','test_cpp_framework_serializer_registry']
profile=wt/'framework/languages/cpp/common/include/zlink/json_profile.hpp'
assert profile.read_bytes()==(r/'raw/candidate-b/include/zlink/json_profile.hpp').read_bytes()
metadata={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=wt,text=True).strip(),'time':datetime.datetime.now(datetime.timezone.utc).isoformat(),'cpus':sorted(os.sched_getaffinity(0)),'policy':os.sched_getscheduler(0),'header_sha256':hashlib.sha256(profile.read_bytes()).hexdigest(),'binaries':{}}
for name in names:
 binary=build/name
 source_name='test_cpp_framework_typed_json_connector' if name in ['test_cpp_framework_typed_json_connector_core','test_cpp_framework_typed_json_connector_extension'] else name
 source=wt/'framework/languages/cpp/tests/Zlink.Framework.UnitTests'/f'{source_name}.cpp'
 assert binary.stat().st_mtime_ns>max(profile.stat().st_mtime_ns,source.stat().st_mtime_ns),(name,'stale')
 metadata['binaries'][name]={'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'mtime':binary.stat().st_mtime_ns,'path':str(binary),'source_mtime':source.stat().st_mtime_ns}
(r/'raw/production-test-metadata.json').write_text(json.dumps(metadata,indent=2))
with(r/'raw/ctest-production.log').open('w')as f:
 p=subprocess.run(['ctest','--test-dir',str(build),'--output-on-failure','-R','^('+ '|'.join(names)+')$'],stdout=f,stderr=subprocess.STDOUT)
assert p.returncode==0,p.returncode
controls=[]
for option in ['--inject-encode-allocation','--inject-decode-allocation']:
 with(r/'raw'/f'allocation-negative{option}.log').open('w')as f:
  p=subprocess.run([str(build/'test_cpp_framework_typed_json_allocations'),option],stdout=f,stderr=subprocess.STDOUT,timeout=30)
 controls.append({'option':option,'exit':p.returncode})
 assert p.returncode!=0,(option,'negative control was not detected')
with(r/'raw/ldd-production.txt').open('w')as f:subprocess.run(['ldd',str(build/'c4dec-registry-production')],stdout=f,check=True)
(r/'raw/verification-summary.json').write_text(json.dumps({'runtime_tests':len(names),'runtime_exit':0,'negative_controls':controls,'format':{'cpp':0,'common':0,'edge_tests':0,'diff_check':0}},indent=2))
print(json.dumps({'runtime_tests':len(names),'controls':controls}))
