from pathlib import Path
j=Path(__file__).parent
for v in ('main','branch'):
 r=j/v/'framework/languages/cpp/framework/src/runtime'
 p=r/'mesh/raw_mesh_node_owner.cpp';s=p.read_text();s='#include "'+str(j/'trace.hpp')+'"\n'+s
 a=s.index('task_t<bool> raw_mesh_node_owner_t::request_instance_spot_activation (');b=s.index('\n}',a)+2;c=s[a:b]
 c=c.replace('    if (timeout <=', '    cdiag::event("activation_submit_enter");\n    if (timeout <=',1)
 c=c.replace('    if (target_routing_id == local.node_routing_id) {','    if (target_routing_id == local.node_routing_id) {\n        cdiag::event("activation_local_enqueue_start");',1)
 c=c.replace('        if (!accepted)\n', '        cdiag::event("activation_local_enqueued");\n        if (!accepted)\n',1)
 s=s[:a]+c+s[b:];p.write_text(s)
 p=r/'stateful/public_host_runtime.cpp';s=p.read_text()
 a=s.index('task_t<void> public_host_runtime_t::dispatch_instance_spot_activation (');b=s.index('\n}',a)+2;c=s[a:b]
 c=c.replace('    auto lifetime = shared_from_this ();','    cdiag::event("activation_dispatch_start");\n    auto lifetime = shared_from_this ();',1)
 c=c.replace('    const auto deliver_terminal = [lifetime, transport = _transport, owned_command, activation,','    const auto deliver_terminal = [lifetime, transport = _transport, owned_command, activation,',1)
 c=c.replace('        instance_spot_activation_result_t result)', '        instance_spot_activation_result_t result)',1)
 s=s[:a]+c+s[b:]
 a=s.index('task_t<void> public_host_runtime_t::dispatch_instance_spot_activation_core (');b=s.index('\n}',a)+2;c=s[a:b]
 c=c.replace('    auto lifetime = shared_from_this ();','    cdiag::event("activation_admission_start");\n    auto lifetime = shared_from_this ();',1)
 c=c.replace('    const auto instance_owner = instance_owner_resolver', '    cdiag::event("activation_admission_configured");\n    const auto instance_owner = instance_owner_resolver',1)
 s=s[:a]+c+s[b:];p.write_text(s)
 p=r/'execution/serial_execution_queue.cpp';s=p.read_text()
 a=s.index('bool serial_execution_queue_t::try_post_async (std::string name,\n');b=s.index('\n}',a)+2;c=s[a:b]
 c=c.replace('{\n','{\n    if (name == "spot-handler") cdiag::event("spot_owner_enqueue_start",this);\n',1)
 s=s[:a]+c+s[b:]
 # Enable the existing executor markers on the actual gate publication path.
 s=s.replace('            cdiag::event("gate_executor_enqueue", this);','            cdiag::event("gate_executor_enqueue", this);\n            cdiag::scope diag_scope(true);',1)
 # Mark a serial owner publication before the release store makes it executable.
 a=s.index('bool serial_execution_queue_t::publish_locked (');b=s.index('\n}',a)+2;c=s[a:b]
 # insert at method entry; the same call owns publication under producer mutex.
 c=c.replace('{\n','{\n    if (node->item && node->item->name == "spot-handler") cdiag::event("spot_owner_publish",this);\n',1)
 s=s[:a]+c+s[b:];p.write_text(s)
