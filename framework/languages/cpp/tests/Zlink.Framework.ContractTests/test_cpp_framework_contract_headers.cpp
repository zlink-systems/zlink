/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>
#include <zlink/framework/version.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/app.hpp>
#include <zlink/framework/contracts/configuration/configuration.hpp>
#include <zlink/framework/contracts/configuration/endpoint_connections.hpp>
#include <zlink/framework/contracts/configuration/lifecycle.hpp>
#include <zlink/framework/contracts/configuration/detail/framework_options_state.hpp>
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>
#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/framework/contracts/configuration/logging.hpp>
#include <zlink/framework/contracts/configuration/mesh_node.hpp>
#include <zlink/framework/contracts/configuration/route_mesh_runtime_options.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/errors/error.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/contracts/eventing/health.hpp>
#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/configuration/transport.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/detail/call_facade.hpp>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/detail/message_payload.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>
#include <zlink/framework/contracts/http/http.hpp>
#include <zlink/framework/contracts/locations/diagnostics.hpp>
#include <zlink/framework/contracts/locations/keys.hpp>
#include <zlink/framework/contracts/locations/location.hpp>
#include <zlink/framework/contracts/locations/options.hpp>
#include <zlink/framework/contracts/locations/resolvers.hpp>
#include <zlink/framework/contracts/locations/rows.hpp>
#include <zlink/framework/contracts/locations/runtime_query.hpp>
#include <zlink/framework/contracts/locations/spot_kind.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/contracts/locations/values.hpp>
#include <zlink/framework/contracts/messaging/message.hpp>
#include <zlink/framework/contracts/messaging/message_context.hpp>
#include <zlink/framework/contracts/monitoring/client_server_runtime.hpp>
#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>
#include <zlink/framework/contracts/placement.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/spots/spot_identity.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <zlink/framework/contracts/timers/timer.hpp>
#include <zlink/framework/contracts/workers/worker.hpp>
#include <zlink/framework/codecs/json.hpp>
#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/framework/codecs/json_stream_e2e_client.hpp>
#include <zlink/codecs/protobuf.hpp>
#include <zlink/http_client.hpp>
#include <zlink/http_client/contracts/client.hpp>
#include <zlink/http_client/contracts/coroutines.hpp>
#include <zlink/http_client/contracts/types.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <zlink/stream_connector_throwing.hpp>
#include <zlink/stream_connector/contracts/calls/zlink_stream_calls.hpp>
#include <zlink/stream_connector/contracts/codec_registry.hpp>
#include <zlink/stream_connector/contracts/compression.hpp>
#include <zlink/stream_connector/contracts/connector.hpp>
#include <zlink/stream_connector/contracts/result.hpp>
#include <zlink/stream_connector/contracts/stream_payload.hpp>
#include <zlink/stream_connector/contracts/version.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_assert.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_connector.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_connector_factory.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_connector_options.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_interfaces.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_models.hpp>
#include <zlink/stream_connector/version.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>
#include <zlink/stream_e2e_client/task.hpp>

#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

struct contract_actor_t;
struct contract_http_client_name_t;

template <typename TContext>
concept has_destroy_actor = requires (TContext & context, contract_actor_t &actor)
{
    context.destroy_actor (actor);
};

template <typename TContext>
concept has_leave_actor = requires (
  TContext & context, const zlink::framework::actor_ref_t &actor_ref, contract_actor_t &actor)
{
    context.leave_actor (actor_ref, actor);
};

template <typename TContext> concept has_run_worker = requires (TContext & context)
{
    context.run_worker ([] { return 1; });
};

template <typename TContext> concept has_split_workers = requires (TContext & context)
{
    context.run_cpu_worker ([] { return 1; });
    context.run_io_worker ([] {
        return zlink::framework::task_t<int> (
          zlink::framework::result_t<int>::success (1));
    });
};

template <typename TRequest> concept has_http_async = requires (TRequest &request)
{
    request.template async<int> ();
};

template <typename TRequest> concept has_http_yield = requires (TRequest &request)
{
    request.template yield<int> ();
};

template <typename TRequest> concept has_http_response_submit = requires (TRequest &request)
{
    request.template submit<int> ();
};

template <typename TRequest> concept has_http_one_way_submit = requires (TRequest &request)
{
    request.submit ();
};

template <typename TRequest> concept has_http_fetch = requires (TRequest &request)
{
    request.template fetch<int> ();
};

template <typename T> concept has_actor_location_spot_kind_member = requires (T value)
{
    value.spot_kind;
};

template <typename T> concept has_actor_location_legacy_generation_member = requires (T value)
{
    value.generation;
};

template <typename T> concept has_actor_location_legacy_location_kind_member = requires (T value)
{
    value.location_kind;
};

template <typename T> concept has_actor_directory_find = requires (T value)
{
    value.find (std::declval<std::string> ());
};

template <typename T> concept has_location_readiness = requires (T value)
{
    value.is_peer_ready (std::declval<std::string> (),
                         zlink::framework::location_role_t::router,
                         std::declval<std::optional<zlink::routing_id_t>> ());
};

static_assert (zlink::framework::version_major == 0);
static_assert (zlink::http_client::version_major == 0);
static_assert (zlink::http_client::version_minor == 3);
static_assert (zlink::http_client::version_patch == 1);
static_assert (zlink::stream_connector::version_major == 0);
static_assert (std::is_same_v<decltype (zlink::stream_e2e_client::use (
                                std::declval<zlink::stream_connector::connector_t &> ())),
                              zlink::stream_e2e_client::coroutine_connector_t>);
static_assert (!std::is_same_v<zlink::framework::task_t<int>, std::future<int>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::request_call_t<int>> ().submit ()),
                 zlink::framework::task_t<int>>);

template <typename T> concept has_blocking_submit = requires (T value)
{
    value.submit ();
};

template <typename T> concept has_yield = requires (T value)
{
    value.yield ();
};

template <typename T, typename TReply> concept has_typed_yield = requires (T value)
{
    value.template yield<TReply> ();
};

template <typename T, typename TReply> concept has_typed_submit = requires (T value)
{
    value.template submit<TReply> ();
};

template <typename T> concept has_legacy_async = requires (T value)
{
    value.async ();
};

template <typename T, typename TReply> concept has_typed_legacy_async = requires (T value)
{
    value.template async<TReply> ();
};

template <typename T> concept has_packet_name = requires (T value)
{
    value.packet_name ("packet");
};

template <typename T> concept has_callback_submit = requires (T value)
{
    value.submit (std::declval<std::function<
                    zlink::framework::task_t<void> (zlink::framework::result_t<int>)>> ());
};

template <typename T> concept has_create_scope = requires (T value)
{
    value.create_scope ();
};

template <typename T> concept has_native_code = requires (T value)
{
    value.native_code ();
};

template <typename T> concept has_raw_monitoring = requires (T value)
{
    value.monitoring ();
};

template <typename T> concept has_raw_metrics = requires (T value)
{
    value.metrics ();
};

static_assert (has_yield<zlink::framework::request_call_t<int>>);

template <typename T> concept has_framework_use_discovery = requires (T value)
{
    value.use_discovery ();
};

template <typename T> concept has_framework_add_registry_peer = requires (T value)
{
    value.add_registry_peer ("tcp://127.0.0.1:5501");
};

template <typename T> concept has_zlink_enable_registry = requires (T value)
{
    value.enable_registry ();
};

template <typename T> concept has_zlink_discovery = requires (T value)
{
    value.discovery ();
};

template <typename T> concept has_spot_node_use_registry_spot_resolver = requires (T value)
{
    value.use_registry_spot_resolver ("route");
};

template <typename T, typename TResult> concept has_callback_async = requires (T value)
{
    value.async ([] (zlink::framework::result_t<TResult>) {});
};

static_assert (has_blocking_submit<zlink::framework::request_call_t<int>>);
static_assert (!has_legacy_async<zlink::framework::request_call_t<int>>);
static_assert (!has_callback_async<zlink::framework::request_call_t<int>, int>);
static_assert (has_blocking_submit<zlink::framework::send_call_t>);
static_assert (!has_callback_async<zlink::framework::send_call_t, void>);
static_assert (!has_yield<zlink::framework::send_call_t>);
static_assert (has_blocking_submit<zlink::framework::relay_request_call_t>);
static_assert (!has_legacy_async<zlink::framework::relay_request_call_t>);
static_assert (
  !has_callback_async<zlink::framework::relay_request_call_t, zlink::framework::message_t>);
static_assert (has_yield<zlink::framework::relay_request_call_t>);
static_assert (has_blocking_submit<zlink::framework::stream_write_call_t>);
static_assert (!has_callback_async<zlink::framework::stream_write_call_t, void>);
static_assert (has_blocking_submit<zlink::framework::route_send_call_t>);
static_assert (!has_callback_async<zlink::framework::route_send_call_t, void>);
static_assert (!has_blocking_submit<zlink::framework::channel_request_call_t>);
static_assert (has_typed_submit<zlink::framework::channel_request_call_t, std::uint64_t>);
static_assert (
  !has_typed_legacy_async<zlink::framework::channel_request_call_t, std::uint64_t>);
static_assert (!has_callback_async<zlink::framework::channel_request_call_t, std::uint64_t>);
static_assert (has_typed_yield<zlink::framework::channel_request_call_t, std::uint64_t>);
static_assert (has_blocking_submit<zlink::framework::actor_send_call_t>);
static_assert (!has_callback_async<zlink::framework::actor_send_call_t, void>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::actor_send_call_t &> ()
                                          .submit ()),
                              zlink::framework::task_t<void>>);
static_assert (!has_blocking_submit<zlink::framework::actor_request_call_t>);
static_assert (
  has_typed_submit<zlink::framework::actor_request_call_t, zlink::framework::message_t>);
static_assert (
  !has_typed_legacy_async<zlink::framework::actor_request_call_t,
                          zlink::framework::message_t>);
static_assert (!has_callback_async<zlink::framework::actor_request_call_t,
                                   zlink::framework::message_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::actor_request_call_t &> ()
                             .submit<zlink::framework::message_t> ()),
                 zlink::framework::task_t<zlink::framework::message_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::actor_request_call_t &> ()
                             .yield<zlink::framework::message_t> ()),
                 zlink::framework::task_t<zlink::framework::message_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::actor_client_t &> ()
                             .send (std::declval<zlink::framework::actor_id_t> (),
                                             std::declval<zlink::framework::message_t> ())),
                 zlink::framework::actor_send_call_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::actor_client_t &> ()
                             .request (std::declval<zlink::framework::actor_id_t> (),
                                                std::declval<zlink::framework::message_t> ())),
                 zlink::framework::actor_request_call_t>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::framework::channel_request_call_t &> ().submit<int> ()),
               zlink::framework::task_t<int>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::publish_call_t &> ().submit ()),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::send_call_t &> ().submit ()),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::route_send_call_t &> ().submit ()),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::bound_session_send_call_t &> ().submit ()),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::stream_send_call_t &> ().submit ()),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::stream_write_call_t &> ().submit ()),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::publisher_t &> ().publish (
                   std::declval<std::string> (), std::declval<std::string> (), int{})),
                 zlink::framework::fanout_publish_call_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::fanout_publish_call_t &> ().submit ()),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::session_actor_t &> ().relay (
                   std::declval<const zlink::message_t &> ())),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::mesh_node_socket_config_t> ()
                .mailbox_message_budget),
    std::uint64_t>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::mesh_node_socket_config_t> ()
                .mailbox_byte_budget),
    std::uint64_t>);

template <typename T> concept has_blocking_wait = requires (T value)
{
    value.wait ();
};

template <typename T> concept has_future_get = requires (T value)
{
    value.get ();
};

static_assert (!has_blocking_wait<zlink::framework::task_t<int>>);
static_assert (!has_future_get<zlink::framework::task_t<int>>);

static_assert (std::is_abstract_v<zlink::framework::route_mesh_runtime_t>);
static_assert (std::is_abstract_v<zlink::framework::mesh_runtime_observation_t>);
static_assert (std::is_abstract_v<zlink::framework::route_mesh_runtime_options_t>);
static_assert (std::is_abstract_v<zlink::framework::mesh_channel_runtime_options_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::route_mesh_runtime_options_t &> ()
                             .placement_weight ()),
                 int>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::route_mesh_runtime_options_t &> ()
                             .channel (std::declval<std::string> ())),
                 zlink::framework::mesh_channel_runtime_options_t &>);
static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::mesh_channel_runtime_options_t &> ()
                             .weight ()),
                 int>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_peer_snapshot_t> ().node_rid),
                 zlink::routing_id_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_snapshot_t> ().peers),
                 std::vector<zlink::framework::mesh_peer_snapshot_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_snapshot_t> ().channels),
                 std::vector<zlink::framework::mesh_channel_snapshot_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_snapshot_t> ()
                             .placement),
                 zlink::framework::mesh_placement_snapshot_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_peer_snapshot_t> ()
                             .unavailable_reason),
                 std::optional<zlink::framework::topology_reason_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::route_mesh_runtime_t &> ()
                             .snapshot (std::declval<std::string> ())),
                 zlink::framework::mesh_node_snapshot_t>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::route_mesh_runtime_t &> ().observe (
      std::declval<std::string> (),
      std::declval<std::size_t> (),
      std::declval<std::function<void (const zlink::framework::observed_status_t<
        zlink::framework::mesh_node_snapshot_t> &)>> ())),
    std::unique_ptr<zlink::framework::mesh_runtime_observation_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::route_mesh_runtime_t &> ()
                             .is_ready (std::declval<std::string> ())),
                 bool>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::app_t &> ().relocate (
      std::declval<zlink::framework::relocation_options_t> (),
      std::declval<std::stop_token> ())),
    zlink::framework::task_t<zlink::framework::relocation_result_t>>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::app_t &> ().shutdown (
      std::declval<std::chrono::milliseconds> (),
      std::declval<std::stop_token> ())),
    zlink::framework::task_t<zlink::framework::termination_result_t>>);
static_assert (
  static_cast<int> (zlink::framework::framework_runtime_state_t::relocating)
  == 2);
static_assert (
  static_cast<int> (zlink::framework::framework_runtime_state_t::relocated)
  == 3);
static_assert (
  static_cast<int> (zlink::framework::termination_outcome_t::force_stopped)
  == 1);

static_assert (std::is_same_v<decltype (zlink::http_client::client_t::create ()
                                          .base_url ("http://127.0.0.1:18080")
                                          .coroutines ()
                                          .build ()
                                          .post ("/sample")),
                              zlink::http_client::request_builder_t>);

static_assert (std::is_polymorphic_v<zlink::http_client::coroutine_execute_scheduler_t>);
static_assert (std::is_polymorphic_v<zlink::http_client::coroutine_resume_scheduler_t>);
static_assert (std::is_base_of_v<zlink::http_client::coroutine_resume_scheduler_t,
                                 zlink::http_client::framework_resume_scheduler_t>);
static_assert (std::is_polymorphic_v<zlink::framework::location_store_t>);
static_assert (
  std::is_polymorphic_v<zlink::framework::relocation_store_t>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<zlink::framework::store_value_t> ()
        .version),
    zlink::framework::store_version_t>);
static_assert (
  std::is_same_v<
    zlink::framework::store_condition_t,
    std::variant<
      zlink::framework::store_missing_condition_t,
      zlink::framework::store_version_condition_t>>);
static_assert (
  std::is_same_v<
    zlink::framework::store_mutation_t,
    std::variant<
      zlink::framework::store_put_t,
      zlink::framework::store_delete_t>>);

template <typename T>
concept exposes_domain_location_operations = requires (T &store) {
    store.claim_owner_lease (
      std::string{}, std::chrono::milliseconds{1});
};

static_assert (
  !exposes_domain_location_operations<
    zlink::framework::location_store_t>);
static_assert (
  std::is_same_v<
    decltype (std::declval<
                zlink::framework::
                  location_store_t &> ()
                .read (
                  std::declval<
                    zlink::framework::store_key_t> ())),
    zlink::framework::task_t<
      zlink::framework::store_read_result_t>>);
static_assert (
  std::is_same_v<
    decltype (std::declval<
                zlink::framework::
                  location_store_t &> ()
                .write (
                  std::declval<
                    zlink::framework::store_write_request_t> ())),
    zlink::framework::task_t<
      zlink::framework::store_write_result_t>>);
static_assert (
  std::is_same_v<
    decltype (std::declval<
                zlink::framework::
                  location_store_t &> ()
                .scan (
                  std::declval<
                    zlink::framework::store_scan_request_t> ())),
    zlink::framework::task_t<
      zlink::framework::store_scan_result_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::zlink_framework_options_t &> ()
                             .add_location_store (
                               std::declval<std::shared_ptr<zlink::framework::location_store_t>> ())),
                 zlink::framework::zlink_framework_options_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::spot_ref_t &> ().spot_id ()),
                 const zlink::framework::spot_id_t &>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::location_runtime_query_t &> ()
                             .get_status ()),
                 zlink::framework::task_t<zlink::framework::location_runtime_status_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::location_runtime_query_t &> ()
                             .list_topology (
                               std::declval<zlink::framework::location_topology_filter_t> (),
                               std::declval<zlink::framework::location_page_request_t> ())),
                 zlink::framework::task_t<
                   zlink::framework::location_page_t<zlink::framework::location_topology_entry_t>>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::location_runtime_query_t &> ()
                             .list_service_summaries (
                               std::declval<zlink::framework::location_service_summary_filter_t> (),
                               std::declval<zlink::framework::location_page_request_t> ())),
                 zlink::framework::task_t<
                   zlink::framework::location_page_t<
                     zlink::framework::location_service_summary_t>>>);
static_assert (has_location_readiness<zlink::framework::location_readiness_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::location_readiness_t &> ()
                             .is_peer_ready (
                               std::declval<std::string> (),
                               zlink::framework::location_role_t::router,
                               std::declval<std::optional<zlink::routing_id_t>> ())),
                 zlink::framework::task_t<bool>>);

static_assert (static_cast<int> (zlink::framework::location_role_t::invalid) == 0);
static_assert (static_cast<int> (zlink::framework::location_role_t::spot) == 2);
static_assert (static_cast<int> (zlink::framework::location_role_t::router) == 3);
static_assert (static_cast<int> (zlink::framework::location_role_t::dealer) == 4);
static_assert (static_cast<int> (zlink::framework::location_role_t::pub) == 5);
static_assert (static_cast<int> (zlink::framework::location_role_t::sub) == 6);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::not_found) == 0);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::already_exists) == 1);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::type_mismatch) == 2);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::not_configured) == 3);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::rejected) == 4);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::unavailable) == 5);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::capacity_exceeded) == 6);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::deadline_exceeded) == 7);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::shutting_down) == 8);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::protocol_error) == 9);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::invalid_operation) == 10);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::data_lost) == 11);
static_assert (static_cast<int> (zlink::framework::framework_error_kind_t::internal_failure) == 12);

static_assert (has_actor_directory_find<zlink::framework::actor_directory_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::actor_directory_t &> ()
                             .find (std::declval<std::string> ())),
                 zlink::framework::task_t<std::optional<zlink::framework::actor_ref_t>>>);
template <typename T>
concept has_http_client_coroutine_resume_builder =
  requires (T value, std::shared_ptr<zlink::http_client::coroutine_resume_scheduler_t> resume)
{
    value.coroutines (resume);
};

template <typename T>
concept has_http_client_coroutine_execute_resume_builder =
  requires (T value,
            std::shared_ptr<zlink::http_client::coroutine_execute_scheduler_t> execute,
            std::shared_ptr<zlink::http_client::coroutine_resume_scheduler_t> resume)
{
    value.coroutines (execute, resume);
};

static_assert (has_http_client_coroutine_resume_builder<zlink::http_client::client_builder_t>);
static_assert (
  has_http_client_coroutine_execute_resume_builder<zlink::http_client::client_builder_t>);

template <typename T> concept has_channel_capability_socket_options = requires (T value)
{
    value.send_high_water_mark (zlink::byte_count_t::bytes (8));
    value.receive_high_water_mark (zlink::byte_count_t::bytes (8));
    value.max_message_size (zlink::byte_size_t::bytes (4096));
    value.peer_weight (zlink::peer_weight_t::value (75));
};

template <typename T>
concept has_legacy_client_server_role_methods = requires (T value)
{
    value.enable_client ();
    value.enable_server ("tcp://127.0.0.1:5000");
};

static_assert (has_channel_capability_socket_options<zlink::framework::capability_builder_t>);
static_assert (
  !has_legacy_client_server_role_methods<
    zlink::framework::client_server_channel_builder_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::channel_runtime_options_t &> ()
                             .client_server_channel ("api")
                             .configure_server_socket ()
                             .peer_weight (zlink::peer_weight_t::value (0))),
                 zlink::framework::channel_server_socket_runtime_options_t &>);

namespace
{

struct named_request_t
{
    static constexpr const char *packet_name = "NamedRequest";
    int value{};
};

struct named_context_request_t
{
    static constexpr const char *packet_name = "NamedContextRequest";
    int value{};
};

struct named_reply_t
{
    int value{};
};

struct contract_actor_t
{
};

struct contract_exact_actor_t : zlink::framework::actor_t
{
    explicit contract_exact_actor_t (
      zlink::framework::actor_context_t context) :
        value (std::move (context))
    {
    }

    zlink::framework::actor_context_t &context () noexcept override
    {
        return value;
    }

    const zlink::framework::actor_context_t &
    context () const noexcept override
    {
        return value;
    }

    zlink::framework::actor_context_t value;
};

struct contract_exact_actor_factory_t
    : zlink::framework::actor_factory_t<contract_exact_actor_t>
{
    zlink::framework::task_t<
      std::shared_ptr<contract_exact_actor_t>>
    create (zlink::framework::actor_context_t context,
            std::stop_token) override
    {
        return zlink::framework::task_t<
          std::shared_ptr<contract_exact_actor_t>> (
          zlink::framework::result_t<
            std::shared_ptr<contract_exact_actor_t>>::success (
              std::make_shared<contract_exact_actor_t> (
                std::move (context))));
    }
};

struct contract_actor_transfer_t
    : zlink::framework::actor_relocation_adapter_t<contract_actor_t>
{
    zlink::framework::task_t<std::vector<std::byte>>
    capture (contract_actor_t &, std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }

    zlink::framework::task_t<void>
    restore (contract_actor_t &,
             std::vector<std::byte>,
             std::stop_token) override
    {
        co_return;
    }
};

struct contract_create_request_t
{
    int value{};
};

struct contract_spot_t
    : public zlink::framework::spot_t<contract_actor_t>
{
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (
      std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }

    void on_create_actor (contract_actor_t &, const zlink::framework::message_t &) {}
    zlink::framework::task_t<void> on_actor_joined (contract_actor_t &) { co_return; }
    zlink::framework::task_t<void> on_leave_actor (contract_actor_t &) { co_return; }
    void on_actor_send (contract_actor_t &,
                        zlink::framework::message_context_t &,
                        const named_request_t &)
    {
    }
    named_reply_t on_actor_request (contract_actor_t &,
                                    zlink::framework::message_context_t &,
                                    const named_request_t &)
    {
        return {};
    }
};

struct contract_instance_spot_t : public zlink::framework::instance_spot_t
{
};

struct contract_context_spot_t
    : public zlink::framework::spot_t<contract_actor_t>
{
    explicit contract_context_spot_t (
      zlink::framework::spot_context_t context) :
        value (std::move (context))
    {
    }
    zlink::framework::spot_context_t &context () noexcept override { return value; }
    const zlink::framework::spot_context_t &context () const noexcept override { return value; }
    void configure () override {}
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }
    zlink::framework::task_t<void>
    on_actor_joined (contract_actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void>
    on_leave_actor (contract_actor_t &) override
    {
        co_return;
    }
    zlink::framework::spot_context_t value;
};

struct contract_context_spot_relocation_adapter_t
    : public zlink::framework::spot_relocation_adapter_t<
        contract_context_spot_t>
{
    zlink::framework::task_t<std::vector<std::byte>>
    capture (contract_context_spot_t &,
             std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }

    zlink::framework::task_t<void>
    restore (contract_context_spot_t &,
             std::vector<std::byte>,
             std::stop_token) override
    {
        co_return;
    }
};

struct contract_context_entry_spot_t
    : public zlink::framework::entry_spot_t<contract_actor_t>
{
    explicit contract_context_entry_spot_t (
      zlink::framework::entry_spot_context_t context) :
        value (std::move (context))
    {
    }
    zlink::framework::entry_spot_context_t &context () noexcept override { return value; }
    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return value;
    }
    void configure () override {}
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }
    zlink::framework::task_t<void>
    on_actor_joined (contract_actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void>
    on_leave_actor (contract_actor_t &) override
    {
        co_return;
    }
    zlink::framework::entry_spot_context_t value;
};

struct contract_context_instance_spot_t : public zlink::framework::instance_spot_t
{
    explicit contract_context_instance_spot_t (
      zlink::framework::instance_spot_context_t context) :
        value (std::move (context))
    {
    }
    zlink::framework::instance_spot_context_t &context () noexcept override { return value; }
    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return value;
    }
    void configure () override {}
    zlink::framework::instance_spot_context_t value;
};

void to_json (nlohmann::json &json, const named_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

void from_json (const nlohmann::json &json, named_request_t &value)
{
    value.value = json.value ("value", 0);
}

void to_json (nlohmann::json &json, const named_context_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

void from_json (const nlohmann::json &json, named_context_request_t &value)
{
    value.value = json.value ("value", 0);
}

void to_json (nlohmann::json &json, const named_reply_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

void from_json (const nlohmann::json &json, named_reply_t &value)
{
    value.value = json.value ("value", 0);
}

zlink::message_t to_stream_payload (const named_request_t &value)
{
    return zlink::message_t::from_json (value);
}

void from_stream_payload (const zlink::message_t &message, named_request_t &value)
{
    value = message.parse_json<named_request_t> ();
}

zlink::message_t to_stream_payload (const named_context_request_t &value)
{
    return zlink::message_t::from_json (value);
}

void from_stream_payload (const zlink::message_t &message, named_context_request_t &value)
{
    value = message.parse_json<named_context_request_t> ();
}

zlink::message_t to_stream_payload (const named_reply_t &value)
{
    return zlink::message_t::from_json (value);
}

void from_stream_payload (const zlink::message_t &message, named_reply_t &value)
{
    value = message.parse_json<named_reply_t> ();
}

struct named_send_handler_t
{
    using message_type = named_request_t;
    void handle (const named_request_t &) {}
};

struct named_request_handler_t
{
    using request_type = named_request_t;
    using reply_type = named_reply_t;
    named_reply_t handle (const named_request_t &) { return {}; }
};

struct named_publish_handler_t
{
    using event_type = named_request_t;
    void handle (const named_request_t &) {}
};

struct named_route_handler_t
{
    named_reply_t handle_request (const named_request_t &,
                                  const zlink::framework::route_message_context_t &)
    {
        return {};
    }

    void handle_send (const named_request_t &, const zlink::framework::route_message_context_t &) {}
};

class named_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::message_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }
};

struct typed_session_payload_t
{
    int value = 0;
};

struct typed_session_handler_t
{
    zlink::framework::task_t<void> handle (zlink::framework::stream_t &,
                                           const typed_session_payload_t &)
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }
};

struct untyped_session_handler_t
{
    void handle (zlink::framework::stream_t &, const typed_session_payload_t &) {}
};

static_assert (zlink::framework::typed_session_packet_handler_for<
               typed_session_handler_t, zlink::framework::stream_t, typed_session_payload_t>);
static_assert (!zlink::framework::typed_session_packet_handler_for<
               untyped_session_handler_t, zlink::framework::stream_t, typed_session_payload_t>);
static_assert (
  std::is_same_v<decltype (zlink::framework::dispatch_typed_session_packet<typed_session_payload_t> (
                   std::declval<typed_session_handler_t &> (),
                   std::declval<zlink::framework::stream_t &> (),
                   std::declval<zlink::framework::serializer_registry_t &> (),
                   std::declval<const zlink::message_t &> ())),
                 zlink::framework::task_t<void>>);

struct typed_config_t
{
    std::string endpoint;

    static typed_config_t bind (const zlink::framework::configuration_section_t &section)
    {
        return {.endpoint = section.require ("endpoint")};
    }
};

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::channel_client_t &> ()
                                          .request ("sample", named_request_t{})),
                              zlink::framework::channel_request_call_t>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::route_send_call_t &> ()
                                          .metadata ("trace-id", "abc")),
                              zlink::framework::route_send_call_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::send_call_t &> ()),
                 zlink::framework::send_call_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::send_call_t &> ().metadata (
                                "trace-id", "abc")),
                              zlink::framework::send_call_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::channel_request_call_t &> ()
                                          ),
                              zlink::framework::channel_request_call_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::channel_request_call_t &> ()
                                          .metadata ("trace-id", "abc")),
                              zlink::framework::channel_request_call_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::channel_request_call_t &> ()
                                          .metadata ("trace-id", "abc")),
                              zlink::framework::channel_request_call_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::stream_write_call_t &> ()
                                          .metadata ("trace-id", "abc")),
                              zlink::framework::stream_write_call_t &>);

static_assert (!has_packet_name<zlink::framework::stream_write_call_t>);
static_assert (has_packet_name<zlink::framework::stream_send_call_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::stream_write_call_t &> ().compress ()),
                 zlink::framework::stream_write_call_t &>);

static_assert (!has_raw_monitoring<zlink::framework::app_t>);
static_assert (!has_raw_metrics<zlink::framework::app_t>);
static_assert (!has_native_code<zlink::framework::stream_error_t>);
static_assert (!has_create_scope<zlink::framework::service_provider_t>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::stream_t &> ().close ()),
                              zlink::framework::task_t<void>>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::stream_t &> ().write_packet (
                                std::declval<const zlink::message_t &> ())),
                              zlink::framework::stream_send_call_t>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::stream_t &> ().reply_packet (
                                std::declval<const zlink::message_t &> ())),
                              zlink::framework::stream_write_call_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::bound_session_t &> ().disconnect ()),
                 zlink::framework::task_t<void>>);
static_assert (!has_yield<zlink::framework::bound_session_send_call_t>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::config_builder_t &> ()
                                          .bind<typed_config_t> ("server")),
                              std::optional<typed_config_t>>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::zlink_framework_options_t &> ().configure_dispatch ()),
    zlink::framework::dispatch_options_t &>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::zlink_framework_options_t &> ().worker ()),
    zlink::framework::worker_options_t &>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::worker_options_t &> ().min_threads ()),
    std::size_t>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::worker_options_t &> ().min_threads (
      std::declval<std::size_t> ())),
    zlink::framework::worker_options_t &>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::worker_options_t &> ().max_threads ()),
    std::size_t>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::worker_options_t &> ().idle_timeout ()),
    std::chrono::milliseconds>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::worker_options_t &> ().max_queue_length ()),
    std::size_t>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<zlink::framework::zlink_framework_options_t &> ()
        .configure_inbound_dispatch ()),
    zlink::framework::inbound_dispatch_options_t &>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<zlink::framework::inbound_dispatch_options_t &> ()
        .set_application_hwm_bytes (
          std::declval<std::optional<std::uint64_t>> ())),
    zlink::framework::inbound_dispatch_options_t &>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<zlink::framework::inbound_dispatch_status_t> ()
        .pending_payload_bytes),
    std::uint64_t>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<zlink::framework::inbound_dispatch_status_t> ()
        .application_receive_paused),
    bool>);
static_assert (
  std::is_abstract_v<zlink::framework::framework_runtime_t>);
static_assert (
  std::is_abstract_v<zlink::framework::runtime_observation_t>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<const zlink::framework::framework_runtime_t &> ()
        .status ()),
    zlink::framework::framework_runtime_status_t>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<zlink::framework::framework_runtime_t &> ()
        .observe (
          std::size_t{1},
          std::declval<std::function<void (
            const zlink::framework::observed_status_t<
              zlink::framework::framework_runtime_status_t> &)>> ())),
    std::unique_ptr<zlink::framework::runtime_observation_t>>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<zlink::framework::inbound_dispatch_options_t &> ()
        .set_application_hwm_profile (
          zlink::framework::application_hwm_profile_t::balanced)),
    zlink::framework::inbound_dispatch_options_t &>);
static_assert (
  std::is_same_v<
    decltype (
      std::declval<zlink::framework::inbound_dispatch_options_t &> ()
        .set_process_memory_limit_bytes (
          std::declval<std::optional<std::uint64_t>> ())),
    zlink::framework::inbound_dispatch_options_t &>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::dispatch_options_t &> ().set_message_flow_observer (
      std::declval<std::shared_ptr<zlink::framework::message_flow_observer_t>> ())),
    zlink::framework::dispatch_options_t &>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::dispatch_options_t &> ().set_message_flow_observer (
      std::declval<std::function<void (const zlink::framework::message_flow_event_t &)>> ())),
    zlink::framework::dispatch_options_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::http_context_t &> ()
                                          .response_header ("X-Test", "value")),
                              zlink::framework::http_context_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::http_response_t &> ()
                                          .header ("X-Test", "value")),
                              zlink::framework::http_response_t &>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::http_options_builder_t &> ().configure_tls (
      std::declval<std::function<void (zlink::framework::http_tls_options_builder_t &)>> ())),
    zlink::framework::http_options_builder_t &>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::http_options_builder_t &> ().configure_server (
      std::declval<std::function<void (zlink::framework::http_server_options_builder_t &)>> ())),
    zlink::framework::http_options_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::http_server_options_builder_t &> ()
                             .set_max_connections (4)),
                 zlink::framework::http_server_options_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::handler_options_builder_t &> ()
                             .group ("api")
                             .add_send<named_send_handler_t> ()),
                 zlink::framework::handler_options_builder_t::group_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::handler_options_builder_t &> ()
                             .group ("events")
                             .add_publish<named_publish_handler_t> ()),
                 zlink::framework::handler_options_builder_t::group_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::stream_node_options_builder_t &> ()
                             .register_session<named_session_t> ()),
                 zlink::framework::stream_node_options_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::stream_node_options_builder_t &> ()
                             .set_tls_server ("server.crt", "server.key", true)),
                 zlink::framework::stream_node_options_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::fanout_channel_builder_t &> ()
                             .enable_publisher ("tcp://127.0.0.1:5000")),
                 zlink::framework::fanout_channel_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::fanout_channel_builder_t &> ()
                             .connect ("tcp://127.0.0.1:5001")),
                 zlink::framework::fanout_channel_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::fanout_channel_builder_t &> ()
                             .use_handler_group ("events")),
                 zlink::framework::fanout_channel_builder_t &>);

static_assert (
  std::is_same_v<
    decltype (
      std::declval<
        zlink::framework::client_server_channel_builder_t &> ()
        .client ()),
    zlink::framework::client_server_channel_client_builder_t>);

static_assert (
  std::is_same_v<
    decltype (
      std::declval<
        zlink::framework::client_server_channel_builder_t &> ()
        .server ()),
    zlink::framework::client_server_channel_server_builder_t>);

static_assert (
  std::is_same_v<
    decltype (
      std::declval<
        zlink::framework::
          client_server_channel_client_builder_t &> ()
        .connect ("tcp://127.0.0.1:5300")),
    zlink::framework::client_server_channel_client_builder_t &>);

static_assert (
  std::is_same_v<
    decltype (
      std::declval<
        zlink::framework::
          client_server_channel_server_builder_t &> ()
        .listen ()
        .set_bind_host ("127.0.0.1")
        .set_advertise_host ("server.example")
        .set_weight (75)
        .add_handler_group ("orders")),
    zlink::framework::client_server_channel_server_builder_t &>);

static_assert (
  std::is_same_v<
    decltype (
      std::declval<
        zlink::framework::
          client_server_channel_server_builder_t &> ()
        .add_send_handler<
          named_send_handler_t, named_request_t> ("send")),
    zlink::framework::client_server_channel_server_builder_t &>);

static_assert (
  std::is_same_v<
    decltype (
      std::declval<
        zlink::framework::
          client_server_channel_server_builder_t &> ()
        .add_request_handler<
          named_request_handler_t, named_request_t,
          named_reply_t> ("request")),
    zlink::framework::client_server_channel_server_builder_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::capability_builder_t &> ()
                                          .set_routing_id (zlink::routing_id_t::from ("api"))),
                              zlink::framework::capability_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::zlink_framework_options_t &> ()
                             .add_fanout_channel ("events")),
                 zlink::framework::fanout_channel_builder_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::route_mesh_channel_builder_t &> ()
                             .enable_server ("tcp://127.0.0.1:5300")),
                 zlink::framework::route_mesh_channel_builder_t &>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::route_mesh_channel_builder_t &> ().enable_client ()),
    zlink::framework::route_mesh_channel_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::route_mesh_channel_builder_t &> ()
                             .enable_client ("tcp://127.0.0.1:5301")),
                 zlink::framework::route_mesh_channel_builder_t &>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::route_mesh_channel_builder_t &> ()
                .add_request_handler<named_route_handler_t, named_request_t, named_reply_t> (
                  "request", &named_route_handler_t::handle_request)),
    zlink::framework::route_mesh_channel_builder_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::route_mesh_channel_builder_t &> ()
                             .add_send_handler<named_route_handler_t, named_request_t> (
                               "send", &named_route_handler_t::handle_send)),
                 zlink::framework::route_mesh_channel_builder_t &>);

static_assert (
  std::has_virtual_destructor_v<
    zlink::framework::spot_t<contract_actor_t>>);
static_assert (
  std::has_virtual_destructor_v<
    zlink::framework::entry_spot_t<contract_actor_t>>);
static_assert (
  !std::is_base_of_v<
    zlink::framework::spot_t<contract_actor_t>,
    zlink::framework::entry_spot_t<contract_actor_t>>);
static_assert (std::has_virtual_destructor_v<
               zlink::framework::actor_relocation_adapter_t<contract_actor_t>>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                           .set_placement_weight (10000)),
               zlink::framework::mesh_node_builder_t &>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                           .set_object_role (
                             zlink::framework::object_role_t::client)),
               zlink::framework::mesh_node_builder_t &>);
static_assert (static_cast<int> (zlink::framework::spot_close_reason_t::idle_evicted)
               == 3);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                           .set_instance_spot_idle_timeout (
                             std::chrono::milliseconds{})),
               zlink::framework::mesh_node_builder_t &>);
static_assert (std::is_same_v<decltype (std::declval<contract_spot_t &> ().on_actor_join (
                                std::declval<std::string_view> (),
                                std::declval<zlink::framework::message_t> ())),
                              zlink::framework::task_t<
                                zlink::framework::spot_actor_join_result_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_context_t &> ().close ()),
                 zlink::framework::task_t<bool>>);
class application_spot_context_t : public zlink::framework::spot_context_t
{
  public:
    application_spot_context_t () = default;
};
static_assert (!std::is_default_constructible_v<application_spot_context_t>);
static_assert (!std::is_copy_constructible_v<zlink::framework::spot_context_t>);
static_assert (std::is_move_constructible_v<zlink::framework::spot_context_t>);
static_assert (!std::is_move_assignable_v<zlink::framework::spot_context_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_context_t &> ().manager ()),
                 zlink::framework::spot_manager_t>);
static_assert (std::is_same_v<decltype (std::declval<const zlink::framework::spot_context_t &> ()
                                         .mesh_name ()),
                              std::string_view>);
static_assert (std::is_same_v<decltype (std::declval<const zlink::framework::spot_context_t &> ()
                                         .object_generation ()),
                              std::uint64_t>);
static_assert (!has_run_worker<zlink::framework::spot_context_t>);
static_assert (has_split_workers<zlink::framework::spot_context_t>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::spot_context_t &> ()
                                          .run_cpu_worker ([] { return 1; })),
                              zlink::framework::worker_call_t<int>>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::spot_context_t &> ()
                                          .run_io_worker ([] {
                                              return zlink::framework::task_t<int> (
                                                zlink::framework::result_t<int>::success (1));
                                          })),
                              zlink::framework::worker_call_t<int>>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::worker_call_t<int> &> ()
                                          .timeout (std::chrono::milliseconds (1))),
                              zlink::framework::worker_call_t<int> &>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::worker_call_t<int> &> ().submit ()),
                 zlink::framework::task_t<int>>);
static_assert (!has_legacy_async<zlink::framework::worker_call_t<int>>);
static_assert (has_yield<zlink::framework::worker_call_t<int>>);
static_assert (!has_callback_submit<zlink::framework::worker_call_t<int>>);
static_assert (has_leave_actor<zlink::framework::spot_context_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_context_t &> ().leave_actor (
                   std::declval<const zlink::framework::actor_ref_t &> (),
                   std::declval<contract_actor_t &> ())),
                 zlink::framework::task_t<zlink::framework::actor_ref_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::session_actor_manager_t &> ()
                             .bind_or_get (std::declval<zlink::framework::actor_ref_t> ())),
                 zlink::framework::request_call_t<zlink::framework::session_actor_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::stream_t &> ().actors ()),
                 zlink::framework::session_actor_manager_t &>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_node_builder_t &> ().snapshot ()),
                 zlink::framework::spot_node_snapshot_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_handler_registry_t &> ()
                             .add_actor_send<&contract_spot_t::on_actor_send> ()),
                 zlink::framework::spot_handler_registry_t &>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_handler_registry_t &> ()
                             .add_actor_request<&contract_spot_t::on_actor_request> ()),
                 zlink::framework::spot_handler_registry_t &>);
static_assert (static_cast<int> (zlink::framework::spot_handler_kind_t::actor_send) == 2);
static_assert (static_cast<int> (zlink::framework::spot_handler_kind_t::actor_request) == 3);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_manager_t &> ()
                             .create (std::declval<std::string> ())),
                 zlink::framework::spot_create_call_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_manager_t &> ()
                             .get_or_create (
                               std::declval<zlink::framework::spot_id_t> (),
                               std::declval<std::string> ())),
                 zlink::framework::spot_create_call_t>);
static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::spot_manager_t &> ()
                             .find (std::declval<zlink::framework::spot_id_t> ())),
                 zlink::framework::task_t<
                   std::optional<zlink::framework::spot_ref_t>>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_manager_t &> ()
                             .close (std::declval<zlink::framework::spot_ref_t> ())),
                 zlink::framework::task_t<bool>>);
static_assert (!std::is_copy_constructible_v<zlink::framework::spot_create_call_t>);
static_assert (std::is_move_constructible_v<zlink::framework::spot_create_call_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_create_call_t &> ()
                             .creation_request (
                               std::declval<zlink::framework::message_t> ())),
                 zlink::framework::spot_create_call_t &>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_create_call_t &> ()
                             .submit ()),
                 zlink::framework::task_t<zlink::framework::spot_create_result_t>>);
static_assert (!has_destroy_actor<zlink::framework::spot_context_t>);
static_assert (has_destroy_actor<zlink::framework::entry_spot_context_t>);
static_assert (!has_run_worker<zlink::framework::entry_spot_context_t>);
static_assert (has_split_workers<zlink::framework::entry_spot_context_t>);
static_assert (!has_http_async<zlink::http_client::request_builder_t>);
static_assert (!has_http_yield<zlink::http_client::request_builder_t>);
static_assert (has_http_response_submit<zlink::http_client::request_builder_t>);
static_assert (!has_http_one_way_submit<zlink::http_client::request_builder_t>);
static_assert (has_http_fetch<zlink::http_client::request_builder_t>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::http_client::request_builder_t &> ()
                           .template fetch<int> ()),
               int>);
static_assert (!has_http_async<zlink::http_client::server_request_builder_t>);
static_assert (has_http_yield<zlink::http_client::server_request_builder_t>);
static_assert (has_http_response_submit<zlink::http_client::server_request_builder_t>);
static_assert (has_http_one_way_submit<zlink::http_client::server_request_builder_t>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::http_client::client_builder_t &> ().build_server (
                 std::declval<std::shared_ptr<zlink::http_client::execution_turn_t>> ())),
               zlink::http_client::server_client_t>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::http_client::client_builder_t &> ()
                           .template build_server<contract_http_client_name_t> (
                             std::declval<std::shared_ptr<
                               zlink::http_client::execution_turn_t>> ())),
               zlink::http_client::named_server_client_t<contract_http_client_name_t>>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::entry_spot_context_t &> ()
                                          .run_cpu_worker ([] { return 1; })),
                              zlink::framework::worker_call_t<int>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::entry_spot_context_t &> ().destroy_actor (
                   std::declval<contract_actor_t &> ())),
                 zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::spot_node_builder_t &> ()
                .add_spot_factory<contract_context_spot_t> (
                  "stage",
                  std::declval<std::function<std::shared_ptr<contract_context_spot_t> (
                    zlink::framework::spot_context_t)>> (),
                  std::declval<std::function<void (
                    zlink::framework::user_spot_factory_builder_t<
                      contract_context_spot_t> &)>> ())),
    zlink::framework::spot_node_builder_t &>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::spot_node_builder_t &> ()
                .add_entry_spot<contract_context_entry_spot_t> (
                  std::declval<std::function<std::shared_ptr<contract_context_entry_spot_t> (
                    zlink::framework::entry_spot_context_t)>> ())),
    zlink::framework::spot_node_builder_t &>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                .add_instance_spot_factory<contract_context_instance_spot_t> (
                  "shopping-cart",
                  std::declval<std::function<std::shared_ptr<
                    contract_context_instance_spot_t> (
                    zlink::framework::instance_spot_context_t)>> (),
                  std::declval<std::function<void (
                    zlink::framework::instance_spot_factory_builder_t<
                      contract_context_instance_spot_t> &)>> ())),
    zlink::framework::mesh_node_builder_t &>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::spot_create_result_t> ().reply),
                 std::optional<zlink::framework::message_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::actor_context_t &> ().join_spot (
                   std::declval<zlink::framework::spot_id_t> (),
                   std::declval<const zlink::framework::message_t &> ())),
                 zlink::framework::actor_join_call_t>);
static_assert (std::is_same_v<decltype (std::declval<const zlink::framework::actor_context_t &> ()
                                         .actor_id ()),
                              const zlink::framework::actor_id_t &>);
static_assert (std::is_same_v<decltype (std::declval<const zlink::framework::actor_context_t &> ()
                                         .object_generation ()),
                              std::uint64_t>);
static_assert (std::is_same_v<decltype (std::declval<const zlink::framework::actor_context_t &> ()
                                         .mesh_name ()),
                              std::string_view>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::actor_join_call_t &> ().defer ()),
                 void>);
static_assert (std::is_same_v<
               decltype (std::declval<contract_exact_actor_t &> ()
                           .on_join_completed (
                             std::declval<const zlink::framework::
                               actor_join_completion_t &> ())),
               zlink::framework::task_t<void>>);
static_assert (std::variant_size_v<
                 zlink::framework::actor_join_completion_t>
               == 3);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                .add_actor_factory<
                  contract_exact_actor_t,
                  contract_exact_actor_factory_t> (
                  "actor",
                  std::declval<std::shared_ptr<
                    contract_exact_actor_factory_t>> (),
                  std::declval<std::function<void (
                    zlink::framework::actor_factory_builder_t<
                      contract_exact_actor_t> &)>> ())),
    zlink::framework::mesh_node_builder_t &>);
static_assert (!has_legacy_async<zlink::framework::actor_join_call_t>);
static_assert (!has_blocking_submit<zlink::framework::actor_join_call_t>);
static_assert (!has_yield<zlink::framework::actor_join_call_t>);
static_assert (!has_typed_yield<zlink::framework::actor_join_call_t, std::string>);
static_assert (!std::is_constructible_v<
               zlink::framework::actor_join_call_t,
               zlink::framework::actor_join_call_t::deferred_fn_t,
               zlink::framework::detail::deferred_barrier_reserver_t>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::session_actor_t &> ().relay (
                                std::declval<const zlink::message_t &> ())),
                              zlink::framework::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::session_actor_t &> ().relay_request (
                   std::declval<const zlink::message_t &> ())),
                 zlink::framework::relay_request_call_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                             .peer_connections ()),
                 zlink::framework::mesh_peer_connections_t &>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_peer_connections_t &> ()
                             .connect ("tcp://127.0.0.1:5503")),
                 void>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_peer_connections_t &> ()
                             .connect (zlink::routing_id_t::from ("peer"),
                                       "tcp://127.0.0.1:5503")),
                 void>);

static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::zlink_framework_options_t &> ()
                             .dispatch_options ()),
                 zlink::framework::dispatch_options_t>);

static_assert (!has_framework_use_discovery<zlink::framework::zlink_framework_options_t>);
static_assert (!has_framework_add_registry_peer<zlink::framework::zlink_framework_options_t>);
static_assert (!has_zlink_enable_registry<zlink::framework::zlink_builder_t>);
static_assert (!has_zlink_discovery<zlink::framework::zlink_builder_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::message_metadata_policy_t &> ()
                             .add_forwarded_metadata_key ("trace-id")),
                 zlink::framework::message_metadata_policy_t &>);

static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::message_metadata_t &> ()
                             .find ("trace-id")),
                 std::optional<std::string_view>>);

static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::message_metadata_t &> ()
                             .contains ("trace-id")),
                 bool>);

static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::message_metadata_t &> ().empty ()),
                 bool>);

static_assert (
  std::is_same_v<decltype (std::declval<const zlink::framework::message_metadata_t &> ().values ()),
                 const std::map<std::string, std::string> &>);

static_assert (std::is_same_v<decltype (zlink::framework::message_context_t::mesh_name),
                              std::optional<std::string>>);
static_assert (std::is_same_v<decltype (zlink::framework::message_context_t::channel_name),
                              std::optional<std::string>>);
static_assert (
  std::is_same_v<decltype (zlink::framework::message_context_t::packet_name), std::string>);
static_assert (std::is_same_v<decltype (zlink::framework::message_context_t::content_type),
                              std::optional<std::string>>);
static_assert (std::is_same_v<decltype (zlink::framework::message_context_t::metadata),
                              zlink::framework::message_metadata_t>);
static_assert (std::is_same_v<decltype (zlink::framework::message_context_t::correlation_id),
                              std::optional<std::string>>);

static_assert (std::is_base_of_v<zlink::framework::message_context_t,
                                 zlink::framework::publish_message_context_t>);
static_assert (
  std::is_same_v<decltype (zlink::framework::publish_message_context_t::topic), std::string>);
static_assert (std::is_same_v<decltype (zlink::framework::publish_message_context_t::source),
                              std::optional<std::string>>);

static_assert (std::is_base_of_v<zlink::framework::message_context_t,
                                 zlink::framework::route_message_context_t>);
static_assert (std::is_same_v<decltype (zlink::framework::route_message_context_t::source_node_rid),
                              zlink::routing_id_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::metadata_policy_builder_t &> ()
                             .add_forwarded_metadata_key ("trace-id")),
                 zlink::framework::metadata_policy_builder_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::config_builder_t &> ()
                                          .bind_required<typed_config_t> ("server")),
                              typed_config_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::config_builder_t &> ().load_json (
                   "appsettings.development.json", zlink::framework::optional_t::yes)),
                 zlink::framework::config_builder_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::config_builder_t &> ()
                                          .use_environment ("development")),
                              zlink::framework::config_builder_t &>);

static_assert (std::is_same_v<decltype (std::declval<const zlink::framework::config_builder_t &> ()
                                          .environment ()),
                              std::string>);

static_assert (std::is_same_v<decltype (std::declval<const zlink::framework::config_builder_t &> ()
                                          .is_environment ("development")),
                              bool>);

class named_handler_t
{
  public:
    named_reply_t handle (const named_request_t &) { return {}; }
    named_reply_t handle_context (const named_context_request_t &,
                                  const zlink::framework::message_context_t &)
    {
        return {};
    }
    void send_context (const named_request_t &, const zlink::framework::message_context_t &) {}
    void publish_context (const named_request_t &,
                          const zlink::framework::publish_message_context_t &)
    {
    }
};

class alias_registered_handler_t
{
  public:
    using request_type = named_request_t;
    using reply_type = named_reply_t;
    static constexpr const char *topic_name = "alias-topic";

    reply_type handle (const request_type &) { return {}; }
};

class named_filter_t
{
  public:
    zlink::framework::task_t<void>
    invoke (const zlink::framework::handler_filter_context_t &,
            zlink::framework::handler_next_t next)
    {
        return next ();
    }
};

static_assert (
  std::is_same_v<zlink::framework::handler_next_t,
                 std::function<zlink::framework::task_t<void> ()>>);
static_assert (
  static_cast<int> (
    zlink::framework::handler_dispatch_kind_t::classic_fanout)
  == 4);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::handler_registry_t &> ()
                                          .use_filter<named_filter_t> ()),
                              zlink::framework::handler_registry_t &>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::route_client_t &> ().send_to_spot (
                                std::declval<zlink::framework::spot_id_t> (),
                                std::declval<named_request_t> ())),
                              zlink::framework::spot_send_call_t>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::route_client_t &> ()
                                         .send_to_channel (
                                           std::declval<std::string> (),
                                           std::declval<named_request_t> ())),
                              zlink::framework::route_send_call_t>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::route_client_t &> ()
                                         .request_to_channel (
                                           std::declval<std::string> (),
                                           std::declval<named_request_t> ())),
                              zlink::framework::channel_request_call_t>);

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::route_client_t &> ()
                                          .request_to_spot (
                                            std::declval<zlink::framework::spot_id_t> (),
                                            std::declval<named_request_t> ())),
                              zlink::framework::spot_request_call_t>);

template <typename T>
concept exposes_retry_hint = requires (const T &value) { value.is_retriable (); };

static_assert (!exposes_retry_hint<zlink::framework::framework_exception_t>);

} // namespace

int main ()
{
    zlink::framework::request_call_t<int> call (zlink::framework::detail::boundary_failure<int> (zlink::framework::detail::boundary_error_t::timed_out, "timeout"));

    auto task = call.submit ();
    const auto coroutine_result = task.result ();
    if (coroutine_result || coroutine_result.error () == nullptr
        || zlink::framework::detail::boundary_state (*coroutine_result.error ())
             != zlink::framework::detail::boundary_error_t::timed_out) {
        return 1;
    }

    zlink::framework::request_call_t<int> shutdown_call (zlink::framework::detail::boundary_failure<int> (zlink::framework::detail::boundary_error_t::shutdown, "shutdown"));

    const auto shutdown_result = shutdown_call.submit ().result ();
    if (shutdown_result || shutdown_result.error () == nullptr
        || zlink::framework::detail::boundary_state (*shutdown_result.error ())
             != zlink::framework::detail::boundary_error_t::shutdown) {
        return 2;
    }

    const auto spot_factory =
      [] (zlink::framework::spot_context_t context) {
          return std::make_shared<contract_context_spot_t> (
            std::move (context));
      };
    bool missing_relocation_rejected = false;
    try {
        zlink::framework::spot_node_builder_t builder;
        builder.add_spot_factory<contract_context_spot_t> (
          "missing-policy", spot_factory, [] (auto &) {});
    }
    catch (const zlink::framework::framework_exception_t &error) {
        missing_relocation_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::not_configured;
    }
    if (!missing_relocation_rejected)
        return 3;

    bool repeated_relocation_rejected = false;
    try {
        zlink::framework::spot_node_builder_t builder;
        builder.add_spot_factory<contract_context_spot_t> (
          "repeated-policy", spot_factory,
          [] (auto &factory) {
              factory.disable_relocation ();
              factory.recreate_on_relocation ();
          });
    }
    catch (const zlink::framework::framework_exception_t &error) {
        repeated_relocation_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::not_configured;
    }
    if (!repeated_relocation_rejected)
        return 4;

    zlink::framework::spot_node_builder_t valid_builder;
    zlink::framework::user_spot_factory_builder_t<
      contract_context_spot_t> *escaped_factory_builder = nullptr;
    valid_builder.add_spot_factory<contract_context_spot_t> (
      "valid-policy", spot_factory,
      [&escaped_factory_builder] (auto &factory) {
          escaped_factory_builder = &factory;
          factory
            .template preserve_state_with<
              contract_context_spot_relocation_adapter_t> ();
      });
    bool sealed_builder_rejected = false;
    try {
        escaped_factory_builder->set_stable_type_limit (10);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        sealed_builder_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::not_configured;
    }
    if (!sealed_builder_rejected)
        return 6;

    bool user_limit_rejected = false;
    try {
        zlink::framework::user_spot_factory_builder_t<
          contract_context_spot_t> builder;
        builder.set_stable_type_limit (0);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        user_limit_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::not_configured;
    }
    bool instance_limit_rejected = false;
    try {
        zlink::framework::instance_spot_factory_builder_t<
          contract_context_instance_spot_t> builder;
        builder.set_stable_type_limit (-1);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        instance_limit_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::not_configured;
    }
    if (!user_limit_rejected || !instance_limit_rejected)
        return 7;

    // The public contract defines exactly 13 kinds and does not expose a retry hint.
    const zlink::framework::framework_error_kind_t error_kind_expectations[] = {
      zlink::framework::framework_error_kind_t::not_found,
      zlink::framework::framework_error_kind_t::already_exists,
      zlink::framework::framework_error_kind_t::type_mismatch,
      zlink::framework::framework_error_kind_t::not_configured,
      zlink::framework::framework_error_kind_t::rejected,
      zlink::framework::framework_error_kind_t::unavailable,
      zlink::framework::framework_error_kind_t::capacity_exceeded,
      zlink::framework::framework_error_kind_t::deadline_exceeded,
      zlink::framework::framework_error_kind_t::shutting_down,
      zlink::framework::framework_error_kind_t::protocol_error,
      zlink::framework::framework_error_kind_t::invalid_operation,
      zlink::framework::framework_error_kind_t::data_lost,
      zlink::framework::framework_error_kind_t::internal_failure};
    static_assert (sizeof (error_kind_expectations) / sizeof (error_kind_expectations[0]) == 13);
    for (std::size_t index = 0; index < std::size (error_kind_expectations); ++index) {
        if (static_cast<std::size_t> (error_kind_expectations[index]) != index)
            return 3;
    }

    zlink::framework::handler_registry_t handlers;
    handlers.on_request<named_handler_t, named_request_t, named_reply_t> ("sample", "topic",
                                                                          &named_handler_t::handle);
    handlers.on_request<named_handler_t, named_context_request_t, named_reply_t> (
      "sample", "context-topic", &named_handler_t::handle_context);
    handlers.on_send<named_handler_t, named_request_t> ("sample", "send-topic",
                                                        &named_handler_t::send_context);
    handlers.on_event<named_handler_t, named_request_t> ("sample", "publish-topic",
                                                         &named_handler_t::publish_context);
    const auto *descriptor = handlers.find ("sample", "topic", named_request_t::packet_name);
    if (descriptor == nullptr || descriptor->packet_name != named_request_t::packet_name) {
        return 5;
    }

    zlink::framework::service_collection_t services;
    zlink::framework::handler_registry_t option_handlers;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t zlink;
    zlink::framework::zlink_framework_options_t options (services, option_handlers, serializers,
                                                         zlink);
    auto &worker_options = options.worker ();
    worker_options.min_threads (2)
      .max_threads (3)
      .idle_timeout (std::chrono::milliseconds (17))
      .max_queue_length (9);
    if (worker_options.min_threads () != 2
        || worker_options.max_threads () != 3
        || worker_options.idle_timeout () != std::chrono::milliseconds (17)
        || worker_options.max_queue_length () != 9) {
        return 8;
    }
    options.use_filter<named_filter_t> ();
    options.handlers ().group ("sample").add<alias_registered_handler_t> ();
    options.apply ();
    bool sealed = false;
    try {
        worker_options.max_queue_length (10);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        sealed = error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!sealed) {
        return 9;
    }

    zlink::framework::capability_builder_t capability;
    const auto capability_snapshot = capability.snapshot ();
    if (!capability_snapshot.max_message_size
        || capability_snapshot.max_message_size->bytes () != 16 * 1024 * 1024) {
        return 6;
    }
    if (zlink::framework::mesh_node_socket_config_t{}.max_message_size
        != 16 * 1024 * 1024) {
        return 7;
    }

    return 0;
}

/* Negative coverage for the MessageContext unification: every per-operation marker context the
 * unification removed must stay removed. Each probe below declares a fallback type with the removed
 * name. If a framework header declared the same name again, the unqualified use would become
 * ambiguous and this translation unit would stop compiling. */
namespace removed_context_probe
{
struct handler_context_t
{
};
struct request_context_t
{
};
struct send_context_t
{
};
struct publish_context_t
{
};
struct handler_invocation_context_t
{
};
struct route_handler_context_t
{
};
struct spot_packet_context_t
{
};
struct spot_actor_send_context_t
{
};
struct spot_actor_request_context_t
{
};
struct spot_actor_reply_options_t
{
};
} // namespace removed_context_probe

using namespace removed_context_probe;
using namespace zlink::framework;

static_assert (sizeof (handler_context_t) == 1);
static_assert (sizeof (request_context_t) == 1);
static_assert (sizeof (send_context_t) == 1);
static_assert (sizeof (publish_context_t) == 1);
static_assert (sizeof (handler_invocation_context_t) == 1);
static_assert (sizeof (route_handler_context_t) == 1);
static_assert (sizeof (spot_packet_context_t) == 1);
static_assert (sizeof (spot_actor_send_context_t) == 1);
static_assert (sizeof (spot_actor_request_context_t) == 1);
static_assert (sizeof (spot_actor_reply_options_t) == 1);
