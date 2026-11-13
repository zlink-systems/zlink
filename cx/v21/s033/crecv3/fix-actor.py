from pathlib import Path
root=Path('/root/worktree/zlink-3867f/framework/languages/cpp/framework')
p=root/'src/runtime/actors/actor_client.cpp';s=p.read_text();a=s.index('        auto copied = parts.items ();',s.index('static task_t<void> submit_prepared_request'));b=s.index('\n    request_task_t<result_t<message_t>>',a);s=s[:a]+'''        try {
            auto copied = parts.items ();
            const auto submit = co_await runtime->request_to_actor (
              actor.native_ref, copied, operation, timeout, {}, actor.authority_owner_generation,
              actor.owner_lease_generation);
            if (submit != zlink::submit_result_t::ok)
                throw runtime::messaging::map_submit_result_exception (submit, "Actor submission");
            receiver.admitted->complete (result_t<void>::success ());
        }
        catch (...) {
            const auto failure = std::current_exception ();
            // Registration consumes operation.receiver. Only a rejection before
            // that transfer has no physical producer and needs local delivery.
            if (operation.receiver)
                receiver.complete (
                  detail::result_access_t::failure<result_t<runtime::messaging::message_parts_t>> (
                    failure));
            receiver.admitted->complete (detail::result_access_t::failure<void> (failure));
        }
    }
''' +s[b:];s=s.replace('''                        receiver.complete (detail::result_access_t::failure<result_t<message_t>> (
                          result.exception ()));
''','');p.write_text(s)
p=root/'src/runtime/mesh/raw_mesh_node_owner.hpp';s=p.read_text();a=s.index('    task_t<bool> request_to_actor (');b=s.index('    task_t<bool> send_bound_session',a);seg=s[a:b].replace('foundation::operation_registry_t::callback_t callback,','foundation::operation_registry_t::callback_t &&callback,');s=s[:a]+seg+s[b:];p.write_text(s)
p=root/'src/runtime/mesh/raw_mesh_node_owner.cpp';s=p.read_text();a=s.index('task_t<bool> raw_mesh_node_owner_t::request_to_actor (');b=s.index('task_t<bool> raw_mesh_node_owner_t::request_user_spot_create',a);seg=s[a:b].replace('foundation::operation_registry_t::callback_t callback,','foundation::operation_registry_t::callback_t &&callback,').replace('co_return co_await request_with_header (','return request_with_header (');s=s[:a]+seg+s[b:];s=s.replace('''              if (!callback)
                  (void) _operations->fail (value.operation,
                                            foundation::operation_terminal_t::transport_failed);
''','''              if (!callback)
                  deliver_request_terminal (_operations, value.operation,
                                            foundation::operation_terminal_t::transport_failed,
                                            detail::current_exception_result<zlink::message_t> (),
                                            receiver);
''',1);s=s.replace('''                        std::move (receiver));
                  }
              }
          }
          catch (...)''','''                        receiver);
                  }
              }
          }
          catch (...)''',1);p.write_text(s)
p=root/'src/runtime/stateful/public_host_runtime.cpp';s=p.read_text();a=s.index('task_t<zlink::submit_result_t> public_host_runtime_t::request_to_actor');b=s.index('public_host_runtime_t::send_to_node',a);seg=s[a:b].replace('const auto accepted = co_await _transport->request_to_actor (','auto submission = _transport->request_to_actor (').replace('    co_return submitted (accepted);','''    // C1 consumes the callback synchronously at publication. The binding/readiness
    // producer now owns physical delivery even if admission later fails.
    if (!complete_public)
        operation.receiver = {};
    const auto accepted = co_await submission;
    co_return submitted (accepted);''');s=s[:a]+seg+s[b:];p.write_text(s)
# Preserve the same unconsumed callback boundary for local Actor registration.
p=root/'src/runtime/mesh/raw_mesh_node_owner.hpp';s=p.read_text();a=s.index('    bool register_prepared_local_operation (');b=s.index(';',a)+1;seg=s[a:b].replace('foundation::operation_registry_t::callback_t callback,','foundation::operation_registry_t::callback_t &&callback,');s=s[:a]+seg+s[b:];p.write_text(s)
p=root/'src/runtime/mesh/raw_mesh_node_owner.cpp';s=p.read_text();a=s.index('bool raw_mesh_node_owner_t::register_prepared_local_operation (');b=s.index('task_t<std::optional<foundation::call_id_t>>',a);seg=s[a:b].replace('foundation::operation_registry_t::callback_t callback,','foundation::operation_registry_t::callback_t &&callback,');s=s[:a]+seg+s[b:];p.write_text(s)
p=root/'src/runtime/stateful/public_host_runtime.cpp';s=p.read_text();a=s.index('std::chrono::steady_clock::time_point public_host_runtime_t::register_local_completion (');b=s.index('bool public_host_runtime_t::enqueue_completion',a);seg=s[a:b];seg=seg.replace('''    const auto registered = _transport->register_prepared_local_operation (
      reservation, deadline,
      [source, result, receiver = std::exchange (operation.receiver, {}),''','''    foundation::operation_registry_t::callback_t complete =
      [source, result, receiver = std::exchange (operation.receiver, {}),''');seg=seg.replace('''      },
      request_surface);
    if (!registered)
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "Operation completion registry is closed");''','''      };
    try {
        if (!_transport->register_prepared_local_operation (
              reservation, deadline, std::move (complete), request_surface))
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Operation completion registry is closed");
    }
    catch (...) {
        // An unconsumed callback never acquired a C1 entry. Its physical
        // receiver still needs the same terminal and capture retirement.
        if (complete)
            complete (foundation::operation_terminal_t::shutdown,
                      result_t<zlink::message_t>::success ({}));
        else
            _transport->fail_local_operation (reservation.first,
                                              foundation::operation_terminal_t::transport_failed);
        throw;
    }''');s=s[:a]+seg+s[b:];p.write_text(s)
