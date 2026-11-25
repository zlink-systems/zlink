/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <google/protobuf/wrappers.pb.h>
#include <zlink/codecs/protobuf.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace zlink::framework::test::actor_gateway
{
int generated_protobuf_bound_session_uses_typed_serializer_codec ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    codec_registration_context_t registration (serializers);
    zlink::framework_codecs::protobuf ().register_framework_codecs (registration);
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("actor-node", "player", "protobuf-bound-session", 7);
    struct observation_t
    {
        std::atomic_bool delivered{false};
        std::string packet_name;
        stream_codec_t codec = stream_codec_t::raw;
        std::string payload;
    };
    const auto observation = std::make_shared<observation_t> ();
    if (!gateway.bind_session_sink (actor, [observation] (std::string packet_name,
                                                          stream_codec_t codec,
                                                          const zlink::message_t &payload) {
            observation->packet_name = std::move (packet_name);
            observation->codec = codec;
            observation->payload = payload.to_string ();
            observation->delivered.store (true, std::memory_order_release);
            return task_t<void> (result_t<void>::success ());
        })) {
        return 1;
    }

    google::protobuf::StringValue message;
    message.set_value ("bound-protobuf");
    const auto expected_payload = message.SerializeAsString ();
    const auto sent =
      gateway.actor_context (actor).bound_session ().send (message).async ().result ();
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!observation->delivered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    if (!sent || !observation->delivered.load (std::memory_order_acquire)) {
        return 2;
    }
    if (observation->packet_name != "StringValue" || observation->codec != stream_codec_t::protobuf
        || observation->payload != expected_payload) {
        return 3;
    }
    return 0;
}
} // namespace zlink::framework::test::actor_gateway
