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
                 "target did not receive the original stamped protobuf body");
        ++count;
    }
};

// Four finite records, no benchmark driver, warmup, throughput or latency result.
void socket_contract (int level)
{
    zlink::context_t context;
    zlink::router_socket_t target (context);
    zlink::router_socket_t source (context);
    const auto target_rid = zlink::routing_id_t::from ("ladder-contract-target");
    target.set_routing_id (target_rid);
    source.set_routing_id (zlink::routing_id_t::from ("ladder-contract-source"));
    target.options ().recv_timeout (std::chrono::seconds (2));
    target.bind ("inproc://ladder-contract");
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
    std::jthread responder ([&] {
        try {
            std::unique_ptr<fw::runtime::application_job_queue_t> jobs;
            if (level >= 1)
                jobs = std::make_unique<fw::runtime::application_job_queue_t> (
                  fw::runtime::resolve_application_job_queue_configuration (
                    fw::application_job_queue_profile_t::balanced, std::nullopt,
                    fw::runtime::detect_application_job_queue_processor_limits ()));
            ladder::fixed_handler_t handler{metrics, level};
            zlink::received_t received;
            for (int record = 0; record < 4; ++record) {
                std::optional<fw::runtime::application_job_queue_t::permit_t> permit;
                if (jobs)
                    permit = jobs->wait_for_supply_blocking (
                      fw::runtime::application_job_queue_t::origin_t::remote);
                require (target.recv (received, zlink::recv_flags_t::none) == 0,
                         "contract target recv failed");
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
        const auto decoded = ladder::decode_reply (reply, level, 456);
        require (decoded.body ().size () == size, "reply body size changed");
        decoded_header_t stamp;
        require (decode_payload (decoded.body ().data (), size, &stamp) && stamp.seq == 456,
                 "source completion lost the protobuf stamp");
        if (level >= 2) {
            if (level >= 3) {
                auto saved = std::move (reply[0]);
                reply[0] = ladder::protocol::encode_reply_header_message (999, 0, 0);
                require_rejected ([&] { ladder::decode_reply (reply, level, 456); },
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
            require_rejected ([&] { ladder::decode_reply (reply, level, 456); },
                              "source completion accepted a malformed envelope");
        }
    }
    responder.join ();
    if (target_failure)
        std::rethrow_exception (target_failure);
    require (metrics.count == 4, "target did not handle all four records");
    std::printf ("L%d socket contract: 1K/4K command and request/reply passed\n", level);
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
    parts[1] = ladder::message (encode_bench_payload (payload));
    fw::detail::channel_reply_writer_t writer;
    auto error = writer.create_error_header ("bench", request,
      fw::framework_exception_t (fw::framework_error_kind_t::internal_failure, "contract error"));
    parts[0] = ladder::messaging::envelope_codec_t{}.encode_header (error);
    require_rejected ([&] { ladder::decode_reply (parts, 2, 0); },
                      "source completion accepted an error envelope");
    std::puts ("reply contract: correlation preserved; malformed envelope/body and error rejected");
}
}

int main ()
{
    try {
        ladder::verify_wire ();
        reply_contract ();
        socket_contract (fw::detail::ablation::level ());
    }
    catch (const std::exception &error) {
        std::fprintf (stderr, "ladder contract: %s\n", error.what ());
        return 1;
    }
}
