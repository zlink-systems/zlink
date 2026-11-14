/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../ladder/ladder_wire.hpp"

#include <cstdio>
#include <exception>
#include <thread>

using namespace zlink_cpp_bench;
namespace fw = zlink::framework;

namespace
{
void require (bool condition, const char *reason)
{
    if (!condition)
        throw std::runtime_error (reason);
}

template <typename Action> void require_rejected (Action action, const char *reason)
{
    bool rejected = false;
    try { action (); }
    catch (const std::exception &) { rejected = true; }
    require (rejected, reason);
}

struct test_metrics_t
{
    int count = 0;
    void record (const void *data, size_t size)
    {
        decoded_header_t stamp;
        require (decode_payload (data, size, &stamp) && stamp.run_id == 123
                   && stamp.seq == 456 && stamp.phase == phase_active
                   && stamp.payload_size == size,
                 "target did not receive the original stamped body");
        ++count;
    }
};

// Four finite records, no benchmark driver, warmup, throughput or latency result.
template <typename Payload = ladder::payload_t>
void socket_contract (int level)
{
    auto context = std::make_shared<zlink::context_t> ();
    std::shared_ptr<fw::runtime::application_job_queue_t> jobs;
    if (level >= 1)
        jobs = std::make_shared<fw::runtime::application_job_queue_t> (
          fw::runtime::resolve_application_job_queue_configuration (
            fw::application_job_queue_profile_t::balanced, std::nullopt,
            fw::runtime::detect_application_job_queue_processor_limits ()));
    zlink::router_socket_t source (*context);
    const auto target_rid = zlink::routing_id_t::from ("ladder-contract-target");
    auto options = ladder::owner_options (target_rid, "inproc://ladder-contract", jobs);
    options.receive_timeout = std::chrono::seconds (2);
    fw::runtime::mesh::raw_mesh_node_owner_t owner (std::move (options), context);
    if (jobs)
        owner.start ();
    else
        owner.ablation_start ();
    auto &target = owner.ablation_socket ();
    source.set_routing_id (zlink::routing_id_t::from ("ladder-contract-source"));
    source.connect ("inproc://ladder-contract");
    zlink::poller_t poller;
    poller.add (source, zlink::poll_event_flag_t::pollcompletion, 1);
    std::jthread completion ([&] (std::stop_token stop) {
        while (!stop.stop_requested ()) {
            zlink::poll_event_t event{};
            poller.wait (&event, 1, std::chrono::milliseconds (10));
        }
    });
    test_metrics_t metrics;
    std::exception_ptr target_failure;
    std::atomic<bool> stop{false};
    std::jthread responder ([&] (std::stop_token token) {
        std::stop_callback cancel (token, [&] { stop = true; owner.signal_activity (); });
        try {
            ladder::fixed_handler_t<test_metrics_t, Payload> handler{metrics, level};
            const auto handle = [&] (zlink::received_t &received,
                                    fw::runtime::application_job_queue_t::permit_t *permit) {
                if (jobs)
                    require (permit && static_cast<bool> (*permit)
                               && jobs->snapshot ().permits_in_use != 0,
                             "target received without a production reservation");
                std::optional<std::uint64_t> correlation;
                require (!ladder::handle_control (target, received,
                           ladder::descriptor (target_rid, "inproc://ladder-contract"),
                           nullptr, &correlation), "unexpected control record");
                if (permit)
                    permit->release_for_handler_entry ();
                const auto *body_storage = received.parts ()[1].data ();
                const auto body_size = received.parts ()[1].size ();
                std::optional<ladder::messaging::envelope_header_t> header;
                auto payload = handler.handle (received, header);
                if (level < 3)
                    require (received.parts ()[1].data () == body_storage,
                             "L2 envelope handling replaced the payload storage");
                else {
                    require (!received.parts ()[1].valid (), "L3 did not move the received frame");
                    require (header && header->retained_header.message.has_value (),
                             "L3 did not retain its borrowed header through reply");
                    const auto retained = reinterpret_cast<std::uintptr_t> (
                      header->retained_header.message->data ());
                    const auto first = reinterpret_cast<std::uintptr_t> (body_storage);
                    require (retained >= first && retained < first + body_size,
                             "L3 copied its unpacked header bytes");
                }
                if (level >= 2) {
                    require (header.has_value (), "target did not retain its parsed envelope");
                    require ((header->kind == ladder::messaging::message_kind_t::request)
                               == received.reply_token ().has_value (),
                             "command/request envelope kind changed");
                    require (header->deadline.has_value () == received.reply_token ().has_value (),
                             "request deadline missing or command gained a deadline");
                    const auto &retained = level >= 3 ? *header->retained_header.message
                                                       : received.parts ()[0];
                    const auto first = reinterpret_cast<std::uintptr_t> (retained.data ());
                    const auto text = reinterpret_cast<std::uintptr_t> (header->channel_name.data ());
                    require (text >= first && text < first + retained.size (),
                             "target parser did not borrow the retained envelope header");
                    auto saved = std::move (received.parts ()[0]);
                    received.parts ()[0] = ladder::message (level >= 3 ? "bad-wire" : "{broken");
                    std::optional<ladder::messaging::envelope_header_t> invalid;
                    std::optional<std::uint64_t> invalid_correlation;
                    require_rejected ([&] {
                        ladder::handle_control (target, received,
                          ladder::descriptor (target_rid, "inproc://ladder-contract"),
                          nullptr, &invalid_correlation);
                        handler.handle (received, invalid);
                    },
                                      "target accepted a malformed wire/envelope header");
                    received.parts ()[0] = std::move (saved);
                }
                if (received.reply_token ())
                    ladder::raw_reply (received, payload, header, correlation);
                if (metrics.count == 4)
                    stop = true;
            };
            if (jobs) {
                const auto evidence = ladder::receive_reserved (owner, jobs, stop, handle);
                require (evidence.received == 4 && evidence.reserved > evidence.received,
                         "target did not reserve a receive batch");
                require (jobs->snapshot ().permits_in_use == 0,
                         "target retained unused receive permits");
            } else {
                zlink::received_t received;
                while (!stop) {
                    require (target.recv (received, zlink::recv_flags_t::none) == 0,
                             "contract target recv failed");
                    handle (received, nullptr);
                }
            }
        }
        catch (...) { target_failure = std::current_exception (); }
    });
    ladder::encoder_t encoder (level);
    for (size_t size : {1024, 4096}) {
        auto command = encoder.encode (size, 123, phase_active, 456, true);
        source.send (target_rid).message (command[0]).message (command[1]).submit ();
        auto request = encoder.encode (size, 123, phase_active, 456, false, std::chrono::seconds (2));
        auto reply = source.request (target_rid).message (request[0]).message (request[1])
                       .timeout (std::chrono::seconds (2)).submit ();
        const auto decoded = ladder::decode_reply<Payload> (reply, level, 456);
        require (decoded.body ().size () == size, "reply body size changed");
        decoded_header_t stamp;
        require (decode_payload (decoded.body ().data (), size, &stamp) && stamp.seq == 456,
                 "source completion lost the body stamp");
        if (level >= 2) {
            if (level >= 3) {
                auto saved = std::move (reply[0]);
                reply[0] = ladder::protocol::encode_reply_header_message (999, 0, 0);
                require_rejected ([&] { ladder::decode_reply<Payload> (reply, level, 456); },
                                  "source accepted the wrong service correlation");
                reply[0] = std::move (saved);
                const auto views = ladder::application_views (reply);
                std::vector<zlink::message_t> invalid;
                invalid.push_back (ladder::message ("{broken"));
                invalid.push_back (zlink::message_t::from (views[1]));
                reply[1] = ladder::protocol::encode_application_payload_message (
                  ladder::protocol::application_payload_t::from_parts (std::move (invalid)));
            } else
                reply[0] = ladder::message ("{broken");
            require_rejected ([&] { ladder::decode_reply<Payload> (reply, level, 456); },
                              "source completion accepted a malformed envelope");
        }
    }
    responder.join ();
    if (target_failure)
        std::rethrow_exception (target_failure);
    require (metrics.count == 4, "target did not handle all four records");
    std::printf ("L%d socket contract: 1K/4K command and request/reply passed\n", level);
}

void receive_pressure_contract (int level)
{
    if (level < 1)
        return;
    using queue_t = fw::runtime::application_job_queue_t;
    auto jobs = std::make_shared<queue_t> (fw::runtime::application_job_queue_configuration_t{
      fw::application_job_queue_profile_t::balanced, 2, 1, 2});
    auto context = std::make_shared<zlink::context_t> ();
    const auto rid = zlink::routing_id_t::from ("permit-contract");
    fw::runtime::mesh::raw_mesh_node_owner_t owner (
      ladder::owner_options (rid, "inproc://permit-contract", jobs), context);
    owner.start ();
    zlink::router_socket_t source (*context);
    source.set_routing_id (zlink::routing_id_t::from ("permit-source"));
    source.connect ("inproc://permit-contract");
    auto source_monitor = source.monitor_open ();
    // Buffer input before applying pressure: synchronous send admission itself
    // waits while the peer is paused, so it must not block this test's releaser.
    for (int index = 0; index < 5; ++index) {
        auto part = ladder::message ("permit");
        source.send (rid).message (part).submit ();
    }
    auto held = jobs->try_reserve_supply_batch (2);
    require (held && held.size () == 2, "could not occupy the queue capacity");
    std::atomic<bool> stop{false};
    std::atomic<unsigned> received{0};
    std::exception_ptr failure;
    ladder::receive_permit_evidence_t evidence;
    std::jthread receiver ([&] (std::stop_token token) {
        std::stop_callback cancel (token, [&] { stop = true; owner.signal_activity (); });
        try {
            evidence = ladder::receive_reserved (owner, jobs, stop,
              [&] (zlink::received_t &record, queue_t::permit_t *permit) {
                  require (permit && static_cast<bool> (*permit)
                               && jobs->snapshot ().permits_in_use != 0,
                           "capacity shortage allowed receive without a permit");
                  require (record.parts ().size () == 1 && record.parts ()[0].to_string () == "permit",
                           "pressure contract payload changed");
                  permit->release_for_handler_entry ();
                  if (++received == 5)
                      stop = true;
              });
        }
        catch (...) { failure = std::current_exception (); stop = true; }
    });
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while ((jobs->snapshot ().capacity_waiters == 0
            || source_monitor.status ().flow_paused_connections == 0) && !stop
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    const bool waited_without_receive = jobs->snapshot ().capacity_waiters == 1 && received == 0;
    const bool peer_paused = source_monitor.status ().flow_paused_connections != 0;
    held.release_without_handler ();
    while (!stop && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    stop = true;
    owner.signal_activity ();
    receiver.join ();
    if (failure)
        std::rethrow_exception (failure);
    require (waited_without_receive, "full queue did not withhold ordinary receive");
    require (peer_paused, "owner queue pressure did not reach the peer socket");
    require (received == 5 && evidence.received == 5, "supply wake did not resume receive");
    const auto status = jobs->snapshot ();
    const auto pressure = jobs->pressure_metrics_snapshot ();
    require (status.capacity_wait_count != 0 && status.permits_in_use == 0
               && status.capacity_waiters == 0, "reservation or waiter leaked");
    require (pressure.paused_transition_count != 0 && pressure.running_transition_count != 0
               && pressure.flow_state_config_failure_count == 0,
             "owner receive-flow pressure application failed");
    while (source_monitor.status ().flow_paused_connections != 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    const auto peer = source_monitor.status ();
    require (peer.flow_paused_connections == 0 && peer.flow_resume_applied_total != 0,
             "owner permit return did not resume the peer socket");
    std::printf ("L%d permit pressure: zero receive while full; supply wake resumed 5 records; "
                 "receive-flow pause/resume; no leaked permits/waiters\n", level);
}

void reply_contract ()
{
    const auto request = ladder::messaging::client_call_codec_t{}.create_envelope (
      ladder::messaging::message_kind_t::request, "bench", "BenchPayload",
      std::chrono::seconds (2), {}, {}, "application/x-protobuf");
    ladder::payload_t payload;
    payload.set_body ("fresh protobuf reply");
    auto raw = ladder::reply_parts (payload, {});
    require (raw[0].to_string () == response_envelope (), "L0/L1 response envelope changed");
    ladder::payload_t raw_payload;
    require (raw_payload.ParseFromArray (raw[1].data (), static_cast<int> (raw[1].size ()))
               && raw_payload.body () == payload.body (),
             "L0/L1 fresh response protobuf changed");
    auto response = ladder::reply_parts (payload, request);
    std::vector<zlink::message_t> parts;
    parts.push_back (std::move (response[0]));
    parts.push_back (std::move (response[1]));
    auto decoded = ladder::messaging::envelope_codec_t{}.decode_header (parts[0], false);
    require (decoded.value ().correlation_id == request.correlation_id
               && decoded.value ().kind == ladder::messaging::message_kind_t::response,
             "reply writer did not preserve request correlation");
    require (ladder::decode_reply (parts, 2, 0).body () == payload.body (),
             "fresh reply protobuf did not roundtrip");
    parts[1] = ladder::message (std::string_view ("\xff", 1));
    require_rejected ([&] { ladder::decode_reply (parts, 2, 0); },
                      "source completion accepted an invalid protobuf body");
    parts[1] = zlink_cpp_bench::encode_bench_payload_message (payload);
    fw::detail::channel_reply_writer_t writer;
    auto error = writer.create_error_header ("bench", request,
      fw::framework_exception_t (fw::framework_error_kind_t::internal_failure, "contract error"));
    parts[0] = ladder::messaging::envelope_codec_t{}.encode_header (error);
    require_rejected ([&] { ladder::decode_reply (parts, 2, 0); },
                      "source completion accepted an error envelope");
    std::puts ("reply contract: correlation preserved; malformed envelope/body and error rejected");
}

void typed_codec_contract ()
{
    const auto serializer = ladder::typed_serializer ();
    require (serializer.content_type () == "application/json", "L4 default serializer is not JSON");
    for (size_t size : {1024, 4096}) {
        auto value = ladder::make_typed_payload (size, 123, phase_active, 456);
        auto encoded = serializer.serialize (value);
        require (encoded.to_string ().starts_with ("{\"body\":\""),
                 "L4 byte body did not use the default base64 JSON field");
        require (serializer.deserialize (encoded).body () == value.body (),
                 "typed JSON lost arbitrary binary bytes");
        auto request = ladder::messaging::client_call_codec_t{}.create_envelope (
          ladder::messaging::message_kind_t::request, "bench", "BenchPayload",
          std::chrono::seconds (2), {}, {}, serializer.content_type ());
        auto response = ladder::reply_parts (value, request);
        auto header = ladder::messaging::envelope_codec_t{}.decode_header (response[0], false).value ();
        require (header.content_type == serializer.content_type ()
                   && header.correlation_id == request.correlation_id,
                 "typed reply lost content type or correlation");
        require (serializer.deserialize (fw::detail::encoded_payload_from_raw (response[1])).body ()
                   == value.body (), "fresh typed reply changed its body");
        for (auto invalid : {"{broken", "{\"body\":123}", "{\"body\":\"!invalid!\"}", "{}"}) {
            auto bad = ladder::message (invalid);
            require_rejected ([&] { serializer.deserialize (fw::detail::encoded_payload_from_raw (bad)); },
                              "typed JSON accepted malformed data");
            std::vector<zlink::message_t> reply;
            reply.push_back (response[0].copy ());
            reply.push_back (bad.copy ());
            require_rejected ([&] { ladder::decode_reply<ladder::typed_payload_t> (reply, 2, 0); },
                              "source completion accepted malformed typed JSON");
        }
        require_rejected ([&] {
            fw::detail::deserialize_typed_payload (serializer, response[1], "application/x-protobuf");
        }, "typed receiver accepted a mismatched content type");
    }
    std::puts ("typed codec: default JSON/base64 roundtrip; invalid JSON/base64/type/content-type rejected");
}

}

int main ()
{
    try {
        ladder::verify_wire ();
        reply_contract ();
        typed_codec_contract ();
        const auto level = fw::detail::ablation::level ();
        receive_pressure_contract (level);
        if (level == 4)
            socket_contract<ladder::typed_payload_t> (level);
        else
            socket_contract (level);
    }
    catch (const std::exception &error) {
        std::fprintf (stderr, "ladder contract: %s\n", error.what ());
        return 1;
    }
}
