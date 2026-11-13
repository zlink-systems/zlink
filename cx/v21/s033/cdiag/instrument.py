from pathlib import Path
j=Path(__file__).parent
inc='#include "'+str(j/'trace.hpp')+'"\n'
for v in ('main','branch'):
 root=j/v/'framework/languages/cpp/framework/src/runtime'
 def edit(path,changes):
  p=root/path;s=p.read_text()
  for a,b in changes:
   if a not in s:raise RuntimeError((v,path,a))
   s=s.replace(a,b,1)
  p.write_text(inc+s)
 edit(Path('stateful/public_host_runtime.cpp'),[
 ('    auto &dispatch = *original;\n    if (!_stateful_dispatch', '    auto &dispatch = *original;\n    if (dispatch.owner.spot_id == "pm-probe-spot") cdiag::event("publish_local");\n    if (!_stateful_dispatch'),
 ('    mesh::service_mailbox_application_t original (std::move (dispatch));\n    auto application = original.retain_mutable ();', '    if (dispatch.owner.spot_id == "pm-probe-spot") cdiag::event("permit_wait_start");\n    mesh::service_mailbox_application_t original (std::move (dispatch));\n    auto application = original.retain_mutable ();'),
 ('          application->application_permit = std::move (*reserved);', '          if (application->owner.spot_id == "pm-probe-spot") cdiag::event("permit_granted");\n          application->application_permit = std::move (*reserved);'),
 ('              dispatch.application_permit.mark_queued ();', '              if (dispatch.owner.spot_id == "pm-probe-spot") cdiag::event("publish_lane_start");\n              dispatch.application_permit.mark_queued ();'),
 ])
 # Restrict insertion to the Spot request function.
 p=root/'stateful/public_host_runtime.cpp';s=p.read_text();a=s.index('public_host_runtime_t::enqueue_local_spot_request (');b=s.index('bool public_host_runtime_t::complete_local_request',a)
 chunk=s[a:b]
 for x,y in [
 ('    auto local = status ();','    cdiag::event("local_request_enter");\n    auto local = status ();'),
 ('          if (!object || object->node_id','          cdiag::event("spot_resolved");\n          if (!object || object->node_id'),
 ('                if (!reservation)','                cdiag::event("completion_reserved");\n                if (!reservation)'),
 ('                          if (!_started || _closing)','                          cdiag::event("lifecycle_lane_start");\n                          if (!_started || _closing)'),
 ('                                ready_record_t owner','                                cdiag::event("local_dispatch_lane_start");\n                                ready_record_t owner')]:
  assert x in chunk,(v,x);chunk=chunk.replace(x,y,1)
 s=s[:a]+chunk+s[b:]
 a=s.index('bool public_host_runtime_t::complete_local_request');b=s.index('result_t<receive_record_t>',a)
 chunk=s[a:b].replace('    receive_record_t completion;','    cdiag::event("reply_completion_enter");\n    receive_record_t completion;',1)
 s=s[:a]+chunk+s[b:];p.write_text(s)
 edit(Path('mesh/service_mailbox.cpp'),[
 ('    std::unique_lock lock (_state->mutex);\n    auto &target = domain (record.domain);','    const bool diag = record.owner.find("pm-probe-spot") != std::string::npos;\n    if (diag) cdiag::event("mailbox_lock_wait");\n    std::unique_lock lock (_state->mutex);\n    if (diag) cdiag::event("mailbox_lock_acquired");\n    auto &target = domain (record.domain);'),
 ('    queue.records.push_back (std::move (record));','    if (diag) cdiag::event("mailbox_prepared");\n    queue.records.push_back (std::move (record));'),
 ('    lock.unlock ();\n    if (notify)\n        notify_application_ready ();\n    return true;', '    if (diag) cdiag::event("owner_enqueued");\n    lock.unlock ();\n    if (notify)\n        notify_application_ready ();\n    return true;'),
 ])
 edit(Path('mesh/mesh_node_host_service.cpp'),[
 ('               registration] (const std::string &owner) {','               registration] (const std::string &owner) {\n                  const bool diag = owner.find("pm-probe-spot") != std::string::npos;\n                  cdiag::scope diag_scope(diag);\n                  if (diag) cdiag::event("owner_ready");'),
 ('        detail::ambient_context_scope_t infrastructure (nullptr, nullptr, {}, false, true);\n        auto pending = drain_application_owner', '        const bool diag = owner.find("pm-probe-spot") != std::string::npos;\n        cdiag::scope diag_scope(diag);\n        if (diag) cdiag::event("owner_executor_start");\n        detail::ambient_context_scope_t infrastructure (nullptr, nullptr, {}, false, true);\n        auto pending = drain_application_owner'),
 ])
 edit(Path('dispatch/offload_executor.cpp'),[
 ('bool offload_executor_t::try_submit_internal (std::function<void ()> work)\n{','bool offload_executor_t::try_submit_internal (std::function<void ()> work)\n{\n    if (cdiag::tagged) cdiag::event("executor_submit_lock_wait");'),
 ('        std::lock_guard lock (_mutex);\n        // Already-admitted', '        std::lock_guard lock (_mutex);\n        if (cdiag::tagged) cdiag::event("executor_submit_lock_acquired");\n        // Already-admitted'),
 # queue push differs main and branch
 ])
 p=root/'dispatch/offload_executor.cpp';s=p.read_text();a=s.index('bool offload_executor_t::try_submit_internal');b=s.index('offload_executor_t::retained_work_t::retained_work_t',a);chunk=s[a:b]
 lines=chunk.splitlines(True)
 for i,line in enumerate(lines):
  if '_queue.push' in line:lines[i]=line+'        if (cdiag::tagged) cdiag::event("executor_enqueued");\n'
 s=s[:a]+''.join(lines)+s[b:];p.write_text(s)
 edit(Path('execution/serial_execution_queue.cpp'),[
 ('void serial_execution_queue_t::run_shared_gate ()\n{','void serial_execution_queue_t::run_shared_gate ()\n{\n    cdiag::event("gate_worker_start", this);'),
 ('    _gate_state.store (gate_state_t::running, std::memory_order_release);\n    auto *previous', '    _gate_state.store (gate_state_t::running, std::memory_order_release);\n    cdiag::event("gate_acquired", this);\n    auto *previous'),
 ('            current_shared_gate = this;\n            const bool published','            cdiag::event("gate_inline_acquired", this);\n            current_shared_gate = this;\n            const bool published'),
 ('            if (_executor.try_submit_internal (\n                  [this, retained = weak_from_this ().lock ()] { run_shared_gate (); }))', '            cdiag::event("gate_executor_enqueue", this);\n            if (_executor.try_submit_internal (\n                  [this, retained = weak_from_this ().lock ()] { run_shared_gate (); }))'),
 ])
pm=(j.with_name('cdrain2')/'bench/pm.hpp').read_text()
pm=inc+pm
pm=pm.replace('  return {request.value};','  cdiag::event("handler_enter");\n  auto result = pm_reply_t{request.value};\n  cdiag::event("handler_exit");\n  return result;')
pm=pm.replace('  const auto start = std::chrono::steady_clock::now ();','  cdiag::begin();\n  const auto start = std::chrono::steady_clock::now ();')
pm=pm.replace('  co_return zlink::framework::http_response_t', '  cdiag::end();\n  co_return zlink::framework::http_response_t')
(j/'pm.hpp').write_text(pm)
