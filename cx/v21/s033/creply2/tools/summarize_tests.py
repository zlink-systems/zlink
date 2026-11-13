from pathlib import Path
import re,json,hashlib,xml.etree.ElementTree as ET
P=Path('/root/jobs/fx/cx/v21/s033/creply2');B=Path('/root/worktree/zlink-3867-creply2/framework/bench/grpc/cpp/build')
row=re.compile(r'iteration=(\d+) close_attempts=(\d+) busy=(\d+) native_after_close=(\d+) retry_gap_ns=(\d+)')
def describe(text,count,closed=True):
 rows=[list(map(int,r)) for r in row.findall(text)]
 assert len(rows)==count,(len(rows),count)
 assert all(r[3]==(402 if closed else 0) for r in rows)
 codes={code:int(n) for code,n in re.findall(r'submit_result=(\d+) count=(\d+)',text)}
 assert codes
 return {'iterations':len(rows),'closed_native_handles':sum(r[3]==402 for r in rows),'busy_calls':sum(r[2] for r in rows),'iterations_with_busy':sum(r[2]>0 for r in rows),'close_attempts_min':min(r[1] for r in rows),'close_attempts_max':max(r[1] for r in rows),'submit_result_codes':codes,'message_ownership_verified':f'message ownership: all {count} iterations consumed or released' in text}
s=(P/'logs/deadline-focused-LastTest.log').read_text();parts=re.split(r'\d+/\d+ Testing: ',s)
blocks={p.splitlines()[0]:p for p in parts[1:]}
result={'focused_race':describe(blocks['test_cpp_framework_reply_close_race'],100),'deterministic':describe(blocks['test_cpp_framework_reply_close_deterministic'],2),'stress':describe((P/'logs/stress-race-1000.log').read_text(),1000),'negative_control':describe((P/'logs/deadline-prototype-destructor-control.log').read_text(),100,False)}
for k,v in result.items():assert v['message_ownership_verified'],k
assert result['deterministic']['busy_calls']==1
assert result['negative_control']['busy_calls']==200
stress=json.loads((P/'stress-status.json').read_text());assert stress['race_exit']==0
result['stress_process']=stress
full=(P/'logs/ctest-unit-contract-LastTest.log').read_text()
full_blocks={p.splitlines()[0]:p for p in re.split(r'\d+/\d+ Testing: ',full)[1:]}
for name,count in [('race',100),('deterministic',2)]:
 key='test_cpp_framework_reply_close_'+name
 if key in full_blocks:result['suite_'+name]=describe(full_blocks[key],count)
root=ET.parse(P/'logs/ctest-unit-contract.xml').getroot();tests=list(root.iter('testcase'))
result['suite']={'tests':len(tests),'failures':sum(t.find('failure') is not None or t.find('error') is not None for t in tests),'skipped':sum(t.find('skipped') is not None for t in tests)}
result['suite']['failed_tests']=[{'name':t.get('name'),'output':t.findtext('system-out','')[:4000]} for t in tests if t.find('failure') is not None or t.find('error') is not None]
if (P/'registry-isolated-status.json').exists():result['registry_isolated']=json.loads((P/'registry-isolated-status.json').read_text())
result['binary_sha256']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in (B/'framework/test_cpp_framework_reply_close',B/'creply_prototype_close_control',B/'framework/libtest_cpp_framework_reply_close_clock.so')}
(P/'test-summary.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k!='binary_sha256'},indent=2))
