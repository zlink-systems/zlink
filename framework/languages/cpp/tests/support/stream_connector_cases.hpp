/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <atomic>
#include <string>

namespace zlink
{
class context_t;
}

namespace zlink::stream_connector
{
class connector_t;
struct connector_options_t;
template <typename T> class result_t;
}

namespace zlink::stream_connector::detail
{
class connector_runtime_t;
}

namespace stream_connector_fixture
{
class joining_thread_t;
struct login_reply_t;
}

namespace stream_connector_tests
{
int test_inbound_codec (const std::string &mode);

int verify_inbound_framing ();

int verify_encode_failure ();

int verify_actor_close_order ();

int verify_completion_before_suspend ();

int verify_reply_and_actor_headers ();

int verify_flow_and_correlation_headers ();

int verify_default_connector ();

int verify_write_order ();

int verify_coroutine_frame_lifetime ();

int verify_metadata_compression_and_payload_limits ();

int verify_typed_connection_setup ();

int verify_connection_lifecycle ();

int verify_wait_connection_loss ();

int verify_connection_callbacks ();

int verify_wait_observations (zlink::context_t &context,
                              const std::string &endpoint,
                              std::atomic_bool &compressed_send_seen,
                              std::atomic_bool &compressible_large_send_seen,
                              std::atomic_bool &uncompressed_send_seen,
                              stream_connector_fixture::joining_thread_t &server_thread,
                              const zlink::stream_connector::connector_options_t &options,
                              zlink::stream_connector::connector_t &connector);

int verify_request_terminal_races ();

int verify_typed_send_and_request (zlink::context_t &context,
                                   const std::string &endpoint,
                                   std::atomic_bool &compressed_send_seen,
                                   std::atomic_bool &compressible_large_send_seen,
                                   std::atomic_bool &uncompressed_send_seen,
                                   stream_connector_fixture::joining_thread_t &server_thread,
                                   const zlink::stream_connector::connector_options_t &options,
                                   zlink::stream_connector::connector_t &connector,
                                   const bool &compressible_large_send_accepted,
                                   zlink::stream_connector::detail::connector_runtime_t &runtime);

int verify_manual_receive (
  zlink::context_t &context,
  const std::string &endpoint,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request);

int verify_immediate_typed_receive (
  zlink::context_t &context,
  const std::string &endpoint,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request);

int verify_public_close (
  zlink::context_t &context,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate);

int verify_automatic_codec (
  zlink::context_t &context,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate);

int verify_request_error_and_close (
  zlink::context_t &context,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate);

int verify_coroutine_operations (
  zlink::context_t &context,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate);

int verify_read_terminal_order ();

int verify_receive_count_and_close_retention ();

int verify_close_during_error_callback (zlink::stream_connector::connector_t &immediate);

int verify_completion_from_callback ();

int verify_write_failure_and_close ();

int verify_immediate_receive_queue ();

int verify_protocol_close ();

int verify_late_receive_handler ();

int verify_inbound_decompression (const zlink::stream_connector::connector_options_t &options,
                                  zlink::stream_connector::connector_t &connector);

int verify_actor_wait_and_dispatch_order (
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector);

int verify_repeated_close_reason (zlink::stream_connector::connector_t &connector);

int verify_late_manual_handler (const zlink::stream_connector::connector_options_t &options,
                                zlink::stream_connector::connector_t &connector);

int verify_decompression_failure ();

int verify_close_from_callback (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate);

int verify_receive_frame_limits (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request);

int verify_heartbeat (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request);

int verify_websocket_and_tls_transport (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime);

int verify_reconnect_and_endpoint_scheme (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  const std::string &websocket_endpoint);

int verify_type_name_and_result ();

int verify_connection_option_validation (zlink::context_t &context);

int verify_arrival_count ();

int verify_subscription_lifetime (zlink::stream_connector::connector_t &connector);

int verify_typed_injection ();

int verify_unlocked_receive_predicate (
  zlink::stream_connector::detail::connector_runtime_t &runtime);

int verify_actor_packet_dispatch ();

int verify_reconnect_jitter ();
} // namespace stream_connector_tests
