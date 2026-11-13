from pathlib import Path
import hashlib,json,os,signal,subprocess,time,shutil
P=Path('/root/jobs/fx/cx/v21/s033/creply');W=Path('/root/worktree/zlink-creply');B=W/'framework/bench/grpc/cpp/build'
mu=P/'runs/main-mutex/mutex-window.json';trigger=False
if mu.exists():
 c=json.loads((mu.parent/'results.json').read_text())['cells'][0];assert c['errors']==c['abandoned']==0 and c['completed']==c['target_stats']['received']
 m=json.loads(mu.read_text());trigger=m['after']['contended']>m['before']['contended']
table=P/'tables.json'
if not trigger and table.exists():
 r=json.loads(table.read_text())['same_rate']
 for i in (1,2):
  f,b=r[f'main-perf-{i}'],r[f'binding-perf-{i}']
  if f['packets_per_request']>b['packets_per_request'] and f['average_data_segment_bytes']<b['average_data_segment_bytes']:trigger=True
assert trigger,'No observed packet increase/smaller segment or mutex contention: do not prototype'
port=W/'framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp';before=(P/'tools/raw_route_port.before.cpp').read_text();assert port.read_text()==before
cm=W/'framework/bench/grpc/cpp/CMakeLists.txt'
base=(P/'tools/CMakeLists.before.txt').read_text()
fixture='''
# Measurement-only close race fixture.
add_executable(creply_close_probe measurement/creply_close_probe.cpp)
bench_target(creply_close_probe)
target_include_directories(creply_close_probe PRIVATE "${BENCH_REPO_ROOT}/framework/languages/cpp/framework/src")
target_link_libraries(creply_close_probe PRIVATE zlink::cpp Threads::Threads)
target_link_options(creply_close_probe PRIVATE -Wl,--wrap=zlink_reply)
'''
assert cm.read_text() in (base,base+fixture)
if cm.read_text()==base:cm.write_text(base+fixture)
def mem():return int(next(l.split()[1] for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
def build(targets,tag):
 assert mem()>=14*1048576,'not enough memory for prototype compile'
 with (P/('build-'+tag+'.log')).open('w') as log,(P/('memory-'+tag+'.jsonl')).open('w') as f:
  child=subprocess.Popen(['cmake','--build',str(B),'--target']+targets+['-j1'],stdout=log,stderr=log,start_new_session=True)
  try:
   while child.poll() is None:
    kb=mem();f.write(json.dumps({'time':time.time(),'available_kb':kb})+'\n');f.flush()
    if kb<13*1048576:os.killpg(child.pid,signal.SIGTERM);child.wait();raise RuntimeError('prototype memory guard')
    time.sleep(.1)
   assert child.returncode==0,child.returncode
  finally:
   if child.poll() is None:os.killpg(child.pid,signal.SIGTERM);child.wait()
build(['creply_close_probe'],'close-main')
shutil.copy2(B/'creply_close_probe',P/'bin-main/creply_close_probe')
head,tail=before.split('bool raw_route_port_t::reply (const raw_received_t &request, std::vector<zlink::message_t> messages)',1)
tail=tail.replace('std::lock_guard lock (*_socket_mutex);','std::unique_lock lock (*_socket_mutex);',1)
tail=tail.replace('''    try {
        std::move (operation).submit ();''','''    // The binding operation has captured its handle and weak runtime token.
    // Core owns submit versus close after C++ socket access ends here.
    lock.unlock ();
    try {
        std::move (operation).submit ();''',1)
port.write_text(head+'bool raw_route_port_t::reply (const raw_received_t &request, std::vector<zlink::message_t> messages)'+tail)
(P/'tools/raw_route_port.direct.cpp').write_text(port.read_text())
with (P/'prototype.diff').open('w') as f:subprocess.run(['git','diff','--','framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp'],cwd=W,stdout=f,check=True)
build(['bench_cpp_framework_server','creply_close_probe'],'direct')
d=P/'bin-direct';d.mkdir(exist_ok=True)
for n in ('bench_cpp_framework_server','creply_close_probe'):shutil.copy2(B/n,d/n)
(P/'binary-direct.json').write_text(json.dumps({n:hashlib.sha256((d/n).read_bytes()).hexdigest() for n in ('bench_cpp_framework_server','creply_close_probe')},indent=2))
(P/'prototype-built.json').write_text(json.dumps({'time':time.time(),'trigger':trigger}))
