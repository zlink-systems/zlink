from pathlib import Path
root=Path('/root/worktree/zlink-3867r/framework/languages/cpp')
p=root/'framework/include/zlink/framework/contracts/dispatch/request_task.hpp';s=p.read_text();i=s.index('    bool has_request_storage () const');s=s[:i]+'''    // Values borrowed by submit and by physical conversion share this request
    // block. The caller retains the alias until submit returns; projections
    // stored in the block borrow it to avoid a cycle.
    template <typename TValue> std::shared_ptr<TValue> retain (TValue value) const
    {
        if (!_storage)
            return std::make_shared<TValue> (std::move (value));
        auto *stored = _storage->store (std::move (value));
        return std::shared_ptr<TValue> (_owner, std::addressof (*stored->value));
    }

'''+s[i:];p.write_text(s)
p=root/'framework/src/runtime/channels/channel_runtime.cpp';s=p.read_text();a=s.index('void route_client_t::submit_request_reply_message_erased');b=s.index('request_task_t<zlink::message_t> route_client_t::submit_channel_request_reply_message_erased',a);part=s[a:b];part=part.replace('        zlink::routing_id_t target;','        std::shared_ptr<const zlink::routing_id_t> target;').replace('target.to_string ()','target->to_string ()');part=part.replace('    request_decoder_t decode{state, transport.value (), target_node_rid, std::move (packet_name),','''    auto target_owner = receiver.complete.retain (std::move (target_node_rid));
    auto target_view = receiver.complete.has_request_storage ()
                         ? std::shared_ptr<const zlink::routing_id_t> (
                             std::shared_ptr<const zlink::routing_id_t>{}, target_owner.get ())
                         : std::shared_ptr<const zlink::routing_id_t> (target_owner);
    request_decoder_t decode{state, transport.value (), std::move (target_view), std::move (packet_name),''');part=part.replace('(*mesh)->request (target_node_rid,','(*mesh)->request (*target_owner,').replace('(*route)->request_reply_parts (target_node_rid,','(*route)->request_reply_parts (*target_owner,');s=s[:a]+part+s[b:];p.write_text(s)
p=root/'tests/Zlink.Framework.UnitTests/test_cpp_framework_request_call_allocations.cpp';s=p.read_text();a=s.index('TEST (RequestCallAllocations, NodeReplyConstructsOnlyFinalPublicAndPhysicalResultStates)');b=s.index('TEST (RequestCallAllocations, NodeProjectionKeepsSubmitTargetWhenInlineReplyReleasesDecoder)',a);part=s[a:b].replace('    std::string submitted_target;','    const zlink::routing_id_t *borrowed_target = nullptr;').replace('        submitted_target = target.to_string ();','        borrowed_target = &target;').replace('    EXPECT_EQ (submitted_target, "n");','''    ASSERT_NE (borrowed_target, nullptr);
    const auto target_allocation = allocation_containing (sample, borrowed_target);
    ASSERT_TRUE (target_allocation.has_value ());
    EXPECT_EQ (borrowed_target->to_string (), "n");''');part=part.replace('    expect_public_and_physical_reply_allocations (sample, *pending, true);','''    expect_public_and_physical_reply_allocations (sample, *pending, true);
    EXPECT_EQ (target_allocation, allocation_containing (sample, &pending->result ()));''');s=s[:a]+part+s[b:];p.write_text(s)
p=root/'framework/src/runtime/foundation/operation_registry.hpp';s=p.read_text().replace('completion_sink_t with_admission (','completion_sink_t &&with_admission (').replace('                             callback_t callback,','                             callback_t &&callback,');p.write_text(s)
p=root/'framework/src/runtime/foundation/operation_registry.cpp';s=p.read_text();s=s.replace('                             callback_t callback,','                             callback_t &&callback,').replace('                                                     callback_t callback,','                                                     callback_t &&callback,').replace('                                               callback_t callback,','                                               callback_t &&callback,');s=s.replace('      std::move (callback), _drain_state, detail::borrow_completion_dispatcher ());','      callback, _drain_state, detail::borrow_completion_dispatcher ());');old='''        } else {
            earlier = index_deadline (id, deadline, *entry);
        }
    }
    if (publication_error)''';new='''        } else {
            earlier = index_deadline (id, deadline, *entry);
        }
        // C1 publication transfers delivery ownership before any later
        // deadline reservation or cancellation can throw or deliver.
        if (accepted)
            callback = {};
    }
    if (publication_error)''';assert old in s;s=s.replace(old,new);p.write_text(s)
p=root/'framework/src/runtime/mesh/raw_mesh_node_owner.cpp';s=p.read_text();a=s.index('          value.registered = _operations->register_operation (',s.index('task_t<bool> raw_mesh_node_owner_t::request_with_header'));b=s.index('          return std::optional<request_start_t>{std::move (value)};',a);s=s[:a]+'''          try {
              value.registered = _operations->register_operation (
                value.operation, deadline, std::move (callback).with_admission (value.admitted),
                peer && peer->ready
                  ? std::vector<std::uint8_t>{}
                  : std::vector<std::uint8_t> (target_routing_id.begin (), target_routing_id.end ()),
                std::move (request_metric), std::move (request), wait_owner);
              if (value.registered) {
                  auto finish = [operations = _operations, operation = value.operation,
                                 correlation = value.correlation, receiver] (auto result) {
                      observe_request (operations, operation, correlation, std::move (result),
                                       receiver);
                  };
                  if (!start_admitted_request (port, peer.get (), target_routing_id, wire, deadline,
                                               value.admitted, finish)) {
                      std::vector<std::uint8_t> target_owner (target_routing_id.begin (),
                                                              target_routing_id.end ());
                      auto peer_observer = observe_request_peer_on_lane (target_owner);
                      (void) submit_registered_request (
                        port, std::move (peer_observer), _operations, value.operation,
                        value.correlation, std::move (target_owner), std::move (wire), {}, deadline,
                        value.admitted->task (), value.admitted, std::move (finish),
                        std::move (receiver));
                  }
              }
          }
          catch (...) {
              // An empty callback was consumed at C1 publication, even if
              // deadline reservation failed before registration returned.
              if (!callback)
                  (void) _operations->fail (value.operation,
                                            foundation::operation_terminal_t::transport_failed);
              throw;
          }
'''+s[b:];p.write_text(s)
