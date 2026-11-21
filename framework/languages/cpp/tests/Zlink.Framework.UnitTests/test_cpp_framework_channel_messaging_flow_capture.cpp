/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include <zlink/framework.hpp>
#include <zlink/codecs/protobuf.hpp>
#include "test_completion_poller_driver.hpp"
#include "runtime/channels/channel_packet_dispatcher.hpp"
#include "runtime/configuration/endpoint_connections.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/channels/channel_host_service.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_bundle.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/route_channel_runtime.hpp"
#include "runtime/channels/route_channel_registration.hpp"
#include "runtime/channels/route_connection_set.hpp"
#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/channels/route_internal_packet_dispatcher.hpp"
#include "runtime/channels/route_packet_dispatcher.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#include "../support/framework_channel_messaging_cases.hpp"


namespace framework_channel_messaging_tests
{
int verify_flow_capture ()
{
    // The Mesh classification decode carries flow capture through route dispatch.
    {
        using namespace zlink::framework;
        namespace msg = runtime::messaging;
        struct flow_command_state_t
        {
            int invocations = 0;
            int last_value = 0;
        };
        struct flow_command_handler_t
        {
            explicit flow_command_handler_t (flow_command_state_t &state) : state (state) {}
            flow_command_state_t &state;
            void handle (const int &value, const route_message_context_t &)
            {
                ++state.invocations;
                state.last_value = value;
            }
        };
        service_collection_t services;
        services.add_singleton<flow_command_state_t> ();
        auto provider = services.build_provider ();
        serializer_registry_t serializers;
        detail::route_handler_registry_t handlers;
        handlers.on_send<flow_command_handler_t, int> ("mesh", "flow-command",
                                                       &flow_command_handler_t::handle);
        auto &handler = provider.get_required<flow_command_state_t> ();
        handler_registry_t filters;
        runtime::host::receive_record_t record;
        record.channel_name = "mesh";
        msg::envelope_header_t header;
        header.kind = msg::message_kind_t::command;
        header.channel_name = "mesh";
        header.message_name = "flow-command";
        header.flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
        header.flow_origin = flow_origin_t::timer;
        msg::envelope_codec_t codec;
        const auto wire = codec.encode_header (header).to_string ();
        auto malformed = wire;
        malformed.replace (malformed.find (*header.flow_id), 4, "ZZZZ");
        auto parts = [] (const std::string &encoded) {
            return std::vector<zlink::message_t>{zlink::message_t::from (encoded),
                                                 zlink::message_t::from (std::string ("42"))};
        };
        dispatch_options_t off_options;
        off_options.message_flow (message_flow_log_mode_t::off);
        detail::mesh_record_dispatcher_t off (provider, serializers, handlers, filters,
                                              off_options);
        if (!off
               .dispatch (
                 record, parts (malformed),
                 codec.decode_header (msg::message_parts_t (parts (malformed)), false).value ())
               .result ()
            || handler.invocations != 1 || handler.last_value != 42)
            return 220;
        dispatch_options_t options;
        options.message_flow (message_flow_log_mode_t::normal);
        auto observations = std::make_shared<
          zlink::framework::test::telemetry_observations_t<message_flow_event_t>> ();
        const auto expected_flow_id = header.flow_id;
        const auto expected_flow_origin = header.flow_origin;
        detail::dispatch_options_access_t::set_observer_for_tests (
          options, [observations] (const message_flow_event_t &event) {
              if (event.outcome == message_flow_outcome_t::received)
                  observations->record (event);
          });
        detail::mesh_record_dispatcher_t on (provider, serializers, handlers, filters, options);
        const auto dispatched =
          on.dispatch (record, parts (wire),
                       codec.decode_header (msg::message_parts_t (parts (wire)), true).value ())
            .result ();
        zlink::framework::test::acknowledge_delivery ();
        const auto observed = observations->snapshot ();
        if (!dispatched || observed.size () != 1 || observed.front ().flow_id != expected_flow_id
            || observed.front ().flow_origin != expected_flow_origin || handler.invocations != 2
            || handler.last_value != 42)
            return 221;
        auto invalid_parts = parts (malformed);
        auto decoded = runtime::decode_mesh_record_header (record, invalid_parts, true);
        if (decoded || decoded.error_kind () != framework_error_kind_t::protocol_error
            || handler.invocations != 2)
            return 222;
    }

    return 0;
}
} // namespace framework_channel_messaging_tests
