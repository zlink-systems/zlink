from pathlib import Path
r=Path('/root/worktree/zlink-cproto/framework/languages/cpp/framework/src/runtime')
p=r/'mesh/raw_mesh_node_owner.hpp';s=p.read_text();a=s.index('    task_t<bool> request_with_header (');b=s.index(';',a);decl=s[a:b+1];s=s[:b+1]+'\n'+decl.replace('task_t<bool> request_with_header','std::shared_ptr<task_completion_source_t<void>> start_request_with_header')+s[b+1:]
i=s.index('    task_t<bool> request_to_channel (',s.index('    task_t<bool> request_to_node ('))
s=s[:i]+'''    void submit_node_request (
      std::span<const std::uint8_t> target_routing_id,
      const protocol::application_payload_t &application_payload,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t &&callback,
      std::shared_ptr<task_completion_source_t<void>> admitted);
'''+s[i:];p.write_text(s)
p=r/'mesh/raw_mesh_node_owner.cpp';s=p.read_text();a=s.index('task_t<bool> raw_mesh_node_owner_t::request_with_header (');b=s.index('\nstruct raw_mesh_node_owner_t::send_completion_state_t',a);old=s[a:b];sig=old[:old.index('\ntry {')];start=old.replace('task_t<bool> raw_mesh_node_owner_t::request_with_header','std::shared_ptr<task_completion_source_t<void>> raw_mesh_node_owner_t::start_request_with_header',1).replace('\ntry {','\n{',1)
start=start[:start.rindex('\ncatch (...)')]
start=start.replace('return task_t<bool> (result_t<bool>::success (false));','return {};').replace('    auto admission = start->admitted->task ();\n    return await_request_admission (std::move (admission));','    return std::move (start->admitted);')
wrapper=sig+'''
try {
    auto source = start_request_with_header (
      target_routing_id, header, application_payload, deadline, std::move (callback),
      requested_correlation, target_claimed, std::move (request_metric), std::move (request),
      std::move (admitted), std::move (receiver));
    if (!source)
        return task_t<bool> (result_t<bool>::success (false));
    return await_request_admission (source->task ());
}
catch (...) {
    return task_t<bool> (detail::current_exception_result<bool> ());
}
'''
s=s[:a]+start+'\n'+wrapper+s[b:]
i=s.index('task_t<bool> raw_mesh_node_owner_t::request_to_channel (')
s=s[:i]+'''void raw_mesh_node_owner_t::submit_node_request (
  std::span<const std::uint8_t> target_routing_id,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t &&callback,
  std::shared_ptr<task_completion_source_t<void>> admitted)
{
    const auto deadline = capture_request_deadline (timeout);
    auto source = start_request_with_header (
      target_routing_id,
      [] (std::uint64_t correlation) {
          return protocol::encode_node_request_header_message (correlation);
      },
      application_payload, deadline, std::move (callback), {}, false,
      mesh_request_metric_t (_request_metrics, mesh_request_surface_t::node), {},
      std::move (admitted));
    if (!source) {
        if (!_topology.peer (target_routing_id))
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "MeshNode request target was not found");
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "MeshNode request was not submitted");
    }
}

'''+s[i:];p.write_text(s)
p=r/'stateful/public_host_runtime.hpp';s=p.read_text();i=s.index('    result_t<receive_record_t> decode_operation_record');s=s[:i]+'''    void submit_node_request (
      const zlink::routing_id_t &target, runtime::messaging::message_parts_t parts,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t &&completion,
      std::shared_ptr<task_completion_source_t<void>> admitted);
'''+s[i:];p.write_text(s)
p=r/'stateful/public_host_runtime.cpp';s=p.read_text();i=s.index('task_t<zlink::submit_result_t>\npublic_host_runtime_t::send_to_channel');s=s[:i]+'''void public_host_runtime_t::submit_node_request (
  const zlink::routing_id_t &target, runtime::messaging::message_parts_t parts,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t &&completion,
  std::shared_ptr<task_completion_source_t<void>> admitted)
{
    _transport->submit_node_request ({target.data (), target.size ()},
      std::move (parts).take_application_payload (), timeout, std::move (completion),
      std::move (admitted));
}

'''+s[i:];p.write_text(s)
p=r/'mesh/mesh_node_runtime.cpp';s=p.read_text();a=s.index('        auto submission = _node->request_to_node (target, std::move (parts), timeout,',s.index('void mesh_node_runtime_t::submit_node_request_reply'));b=s.index('        observe_task_terminal (submission, std::move (submission_observer));',a)+len('        observe_task_terminal (submission, std::move (submission_observer));')
s=s[:a]+'''        _node->submit_node_request (target, std::move (parts), timeout,
                                    std::move (completion), receiver.admitted);
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        // Exercise failure after C1 transfer at the direct admission connection.
        if (auto fault = test_creation_probe::node_admission_observer_fault.exchange (nullptr))
            fault ();
        if (auto fault = test_creation_probe::node_admission_observer_context_fault.exchange (nullptr))
            fault ();
#endif'''+s[b:];p.write_text(s)

p=r/'mesh/raw_mesh_node_owner.hpp';s=p.read_text();marker='    void submit_node_request ('
s=s.replace(marker,'    zlink::submit_result_t node_request_submission_result (\n      std::span<const std::uint8_t> target_routing_id, bool accepted) const;\n'+marker,1);p.write_text(s)
p=r/'mesh/raw_mesh_node_owner.cpp';s=p.read_text();a=s.index('    if (!source) {',s.index('void raw_mesh_node_owner_t::submit_node_request ('));b=s.index('\n}',a)
s=s[:a]+'    if (!source)\n        throw messaging::map_node_request_submit_exception (\n          node_request_submission_result (target_routing_id, false));'+s[b:]
i=s.index('void raw_mesh_node_owner_t::submit_node_request (')
s=s[:i]+'zlink::submit_result_t raw_mesh_node_owner_t::node_request_submission_result (\n  std::span<const std::uint8_t> target_routing_id, bool accepted) const\n{\n    if (accepted)\n        return zlink::submit_result_t::ok;\n    // The error model classifies a rejected target absent from the live peer\n    // table as NotFound. An expected descriptor alone is not a live peer.\n    return _topology.peer (target_routing_id) ? zlink::submit_result_t::not_connected\n                                             : zlink::submit_result_t::not_found;\n}\n\n'+s[i:];p.write_text(s)
p=r/'stateful/public_host_runtime.cpp';s=p.read_text();a=s.index('        const auto accepted = result.value ();',s.index('public_host_runtime_t::request_to_node (const zlink::routing_id_t &target,',s.index('public_host_runtime_t::request_to_node (const zlink::routing_id_t &target,')+1));b=s.index('\n    };',a)
s=s[:a]+'        return result_t<zlink::submit_result_t>::success (\n          host->_transport->node_request_submission_result (\n            {target.data (), target.size ()}, result.value ()));'+s[b:];p.write_text(s)
p=r/'mesh/mesh_node_runtime.cpp';s=p.read_text();a=s.index('node_reply_t node_submission_failure (');b=s.index('\n}',a)+2
s=s[:a]+'node_reply_t node_submission_failure (zlink::submit_result_t submitted)\n{\n    return detail::result_access_t::failure<runtime::messaging::message_parts_t> (\n      runtime::messaging::map_node_request_submit_exception (submitted));\n}'+s[b:];p.write_text(s)
p=r/'messaging/submit_result_mapper.hpp';s=p.read_text();i=s.index('inline framework_exception_t map_request_result_exception (')
s=s[:i]+'inline framework_exception_t map_node_request_submit_exception (zlink::submit_result_t result)\n{\n    if (result == zlink::submit_result_t::not_found)\n        return framework_exception_t (framework_error_kind_t::not_found,\n                                      "MeshNode request target was not found");\n    if (result == zlink::submit_result_t::terminated)\n        return detail::make_boundary_exception (detail::boundary_error_t::shutdown,\n                                                "MeshNode request runtime is stopped");\n    return framework_exception_t (map_submit_result_error_kind (result),\n                                  "MeshNode request was not submitted");\n}\n\n'+s[i:];p.write_text(s)
