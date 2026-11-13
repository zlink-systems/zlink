from pathlib import Path
r=Path('/root/worktree/zlink-v21-s033/p0')
p=r/'framework/languages/cpp/framework/include/zlink/framework/detail/path_counters.hpp';s=p.read_text()
s=s.replace('request_snapshot_pending, request_attempt, send_attempt, total_count','request_snapshot_pending, request_attempt, send_attempt, worker_startup_records, total_count')
s=s.replace('"request_snapshot_pending", "request_attempt", "send_attempt"','"request_snapshot_pending", "request_attempt", "send_attempt", "worker_startup_records"')
s=s.replace('''    std::uint64_t records=0; std::uint64_t *previous;
    turn_t():previous(local().turn_records) { local().turn_records=&records; }
    ~turn_t() { local().turn_records=previous; sample(worker_turn,records); }''','''    std::uint64_t records=0; std::uint64_t *previous; bool enabled;
    explicit turn_t(bool enabled):previous(nullptr),enabled(enabled) {
        if(enabled) { previous=local().turn_records; local().turn_records=&records; }
    }
    ~turn_t() { if(enabled) { local().turn_records=previous; sample(worker_turn,records); } }''')
s=s.replace('''    std::uint64_t records=0; bool active=false;
    wake_t() { local().wake_records=&records; }
    void sleep() { if(active) sample(worker_wake,records); active=false; records=0; }
    void woke(bool ready) { if(ready) { increment(worker_wait_ready); active=true; } }
    ~wake_t() { sleep(); local().wake_records=nullptr; }''','''    std::uint64_t records=0; bool active=false, enabled;
    explicit wake_t(bool enabled):enabled(enabled) { if(enabled) local().wake_records=&records; }
    void sleep() {
        if(!enabled) return;
        if(active) sample(worker_wake,records); else local().totals[worker_startup_records]+=records;
        active=false; records=0;
    }
    void woke(bool ready) { if(enabled && ready) { increment(worker_wait_ready); active=true; } }
    ~wake_t() { if(enabled) { sleep(); local().wake_records=nullptr; } }''')
p.write_text(s)
p=r/'framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.cpp';s=p.read_text()
s=s.replace('path_counters::increment(path_counters::worker_notify);','if(_thread_name == "zlink-mesh-app") path_counters::increment(path_counters::worker_notify);')
s=s.replace('path_counters::wake_t path_wake;', 'path_counters::wake_t path_wake(_thread_name == "zlink-mesh-app");')
s=s.replace('if(path_wait) { path_wake.sleep(); path_counters::increment(path_counters::worker_wait); }','if(path_wait && path_wake.enabled) { path_wake.sleep(); path_counters::increment(path_counters::worker_wait); }')
s=s.replace('path_counters::turn_t path_turn;','path_counters::turn_t path_turn(path_wake.enabled);')
p.write_text(s)
