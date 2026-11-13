from pathlib import Path
r=Path('/root/worktree/zlink-v21-s033/p0')
p=r/'framework/languages/cpp/framework/include/zlink/framework/detail/path_counters.hpp';s=p.read_text().replace('worker_turn, worker_wake, rotation, owner_drain','worker_turn, worker_wake, rotation, rotation_activity, owner_drain').replace('"worker_wake", "rotation",\n    "owner_drain"','"worker_wake", "rotation", "rotation_activity",\n    "owner_drain"');p.write_text(s)
p=r/'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp';s=p.read_text().replace('path_counters::sample(path_counters::rotation,count)', 'path_counters::sample(path_counters::rotation_activity,count)');p.write_text(s)
p=r/'framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp';s=p.read_text();s='#include <zlink/framework/detail/path_counters.hpp>\n'+s
a='''    receive_batch_budget_t budget;
    // One poll per receive turn''';b='''    ZLINK_PATH_COUNT(struct path_rotation_t {
        std::uint64_t records=0;
        ~path_rotation_t() { path_counters::sample(path_counters::rotation,records); }
    } path_rotation;)
    receive_batch_budget_t budget;
    // One poll per receive turn''';assert a in s;s=s.replace(a,b,1)
a='''        budget.account (_transport->last_pump_bytes ());
        trace_mesh_host''';b='''        ZLINK_PATH_COUNT(if(pumped == mesh::raw_mesh_pump_result_t::application) ++path_rotation.records;)
        budget.account (_transport->last_pump_bytes ());
        trace_mesh_host''';assert a in s;s=s.replace(a,b,1);p.write_text(s)
