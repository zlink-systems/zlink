/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zlink
{
class context_t;
class routing_id_t;
}

namespace zlink::framework
{
class handler_registry_t;
class route_client_t;
class serializer_registry_t;
class service_provider_t;
class zlink_builder_t;
struct dispatch_options_t;
struct message_dispatch_error_event_t;
}

namespace zlink::framework::detail
{
class channel_packet_dispatcher_t;
class channel_runtime_t;
class route_channel_runtime_t;
}

namespace zlink::framework::runtime::messaging
{
class envelope_codec_t;
struct envelope_header_t;
class message_parts_t;
}

namespace framework_channel_messaging_tests
{
int verify_flow_capture ();

int verify_channel_configuration ();

int verify_dispatch_observer_setup (zlink::framework::zlink_builder_t &zlink,
                                    zlink::framework::zlink_builder_t &outbound_only,
                                    zlink::framework::detail::channel_runtime_t &outbound_runtime);

int verify_outbound_submission (
  zlink::framework::zlink_builder_t &zlink,
  zlink::framework::zlink_builder_t &outbound_only,
  zlink::framework::detail::channel_runtime_t &outbound_runtime,
  zlink::framework::zlink_builder_t &local_server,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  const std::filesystem::path &dispatch_log_path,
  zlink::framework::dispatch_options_t &local_dispatch);

int verify_channel_dispatch (
  zlink::framework::zlink_builder_t &zlink,
  zlink::framework::detail::channel_runtime_t &outbound_runtime,
  zlink::framework::zlink_builder_t &local_server,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  const std::filesystem::path &dispatch_log_path,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  std::atomic_int &request_deserialize_count);

int verify_hosted_dispatch (
  zlink::framework::zlink_builder_t &zlink,
  zlink::framework::detail::channel_runtime_t &outbound_runtime,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::handler_registry_t &handlers,
  zlink::framework::detail::channel_runtime_t &local_runtime,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  zlink::framework::runtime::messaging::message_parts_t &request_parts,
  zlink::framework::runtime::messaging::message_parts_t &route_request_parts,
  zlink::framework::detail::channel_packet_dispatcher_t &packet_dispatcher,
  std::vector<zlink::framework::message_dispatch_error_event_t> &observed_dispatch_errors);

int verify_channel_receive (
  zlink::framework::zlink_builder_t &zlink,
  zlink::framework::detail::channel_runtime_t &outbound_runtime,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::handler_registry_t &handlers,
  zlink::framework::detail::channel_runtime_t &local_runtime,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  zlink::framework::runtime::messaging::message_parts_t &request_parts,
  zlink::framework::runtime::messaging::message_parts_t &route_request_parts,
  zlink::framework::detail::channel_packet_dispatcher_t &packet_dispatcher,
  std::vector<zlink::framework::message_dispatch_error_event_t> &observed_dispatch_errors);

int verify_route_submission (
  zlink::framework::zlink_builder_t &zlink,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  zlink::framework::runtime::messaging::message_parts_t &request_parts,
  zlink::framework::runtime::messaging::message_parts_t &route_request_parts,
  std::vector<zlink::framework::message_dispatch_error_event_t> &observed_dispatch_errors);

int verify_route_dispatch (
  zlink::framework::zlink_builder_t &zlink,
  std::vector<zlink::framework::message_dispatch_error_event_t> &dispatch_errors,
  std::mutex &dispatch_errors_mutex,
  zlink::framework::dispatch_options_t &local_dispatch,
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  zlink::framework::runtime::messaging::message_parts_t &route_request_parts,
  std::vector<zlink::framework::message_dispatch_error_event_t> &observed_dispatch_errors,
  zlink::framework::detail::route_channel_runtime_t &route_runtime,
  const zlink::routing_id_t &target_node,
  const std::string &target_spot);

int verify_public_route_submission (
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::runtime::messaging::envelope_header_t &request_header,
  const zlink::routing_id_t &target_node,
  const std::string &target_spot);

int verify_spot_routing (zlink::framework::service_provider_t &provider,
                         zlink::framework::serializer_registry_t &serializers,
                         zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
                         const zlink::routing_id_t &target_node,
                         const std::string &target_spot,
                         zlink::framework::detail::route_channel_runtime_t &public_route,
                         zlink::framework::route_client_t &public_route_client);

int verify_live_route_submission (
  zlink::framework::service_provider_t &provider,
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec,
  zlink::framework::detail::route_channel_runtime_t &public_route,
  zlink::framework::route_client_t &public_route_client);

int verify_actor_dispatch (zlink::framework::service_provider_t &provider,
                           zlink::framework::serializer_registry_t &serializers,
                           zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec);

int verify_released_route_client (
  zlink::framework::serializer_registry_t &serializers,
  zlink::framework::runtime::messaging::envelope_codec_t &envelope_codec);
} // namespace framework_channel_messaging_tests
