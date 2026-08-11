/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include "addon_common_api.h"

napi_value version (napi_env env, napi_callback_info info);
napi_value errno_value (napi_env env, napi_callback_info info);
napi_value strerror_value (napi_env env, napi_callback_info info);
napi_value has (napi_env env, napi_callback_info info);
napi_value proxy (napi_env env, napi_callback_info info);
napi_value proxy_steerable (napi_env env, napi_callback_info info);
napi_value sleep (napi_env env, napi_callback_info info);
napi_value message_from_buffer (napi_env env, napi_callback_info info);
napi_value message_allocate (napi_env env, napi_callback_info info);
napi_value message_frame_data (napi_env env, napi_callback_info info);
napi_value message_frame_size (napi_env env, napi_callback_info info);
napi_value message_frame_close (napi_env env, napi_callback_info info);

napi_value ctx_new (napi_env env, napi_callback_info info);
napi_value ctx_shutdown (napi_env env, napi_callback_info info);
napi_value ctx_term (napi_env env, napi_callback_info info);
napi_value ctx_setopt (napi_env env, napi_callback_info info);
napi_value ctx_getopt (napi_env env, napi_callback_info info);
napi_value ctx_getopt_data (napi_env env, napi_callback_info info);
napi_value ctx_recalculate_auto_hwm (napi_env env, napi_callback_info info);

napi_value socket_new (napi_env env, napi_callback_info info);
napi_value socket_close (napi_env env, napi_callback_info info);
napi_value socket_bind (napi_env env, napi_callback_info info);
napi_value socket_unbind (napi_env env, napi_callback_info info);
napi_value socket_connect (napi_env env, napi_callback_info info);
napi_value socket_disconnect (napi_env env, napi_callback_info info);
napi_value socket_disconnect_rid (napi_env env, napi_callback_info info);
napi_value socket_disconnect_transport_pair (napi_env env, napi_callback_info info);
napi_value socket_set_tls_server (napi_env env, napi_callback_info info);
napi_value socket_set_tls_client (napi_env env, napi_callback_info info);
napi_value socket_send (napi_env env, napi_callback_info info);
napi_value socket_send_parts (napi_env env, napi_callback_info info);
napi_value socket_publish (napi_env env, napi_callback_info info);
napi_value socket_try_publish (napi_env env, napi_callback_info info);
napi_value socket_try_send (napi_env env, napi_callback_info info);
napi_value socket_try_send_parts (napi_env env, napi_callback_info info);
napi_value socket_try_send_routing (napi_env env, napi_callback_info info);
napi_value socket_try_send_routing_parts (napi_env env, napi_callback_info info);
napi_value socket_send_routing (napi_env env, napi_callback_info info);
napi_value socket_send_routing_parts (napi_env env, napi_callback_info info);
napi_value socket_recv_message (napi_env env, napi_callback_info info);
napi_value socket_try_recv_message (napi_env env, napi_callback_info info);
napi_value socket_try_recv_message_batch (napi_env env, napi_callback_info info);
napi_value socket_subscribe_message (napi_env env, napi_callback_info info);
napi_value socket_try_subscribe_message (napi_env env, napi_callback_info info);
napi_value socket_try_subscribe_message_batch (napi_env env, napi_callback_info info);
napi_value socket_send_ready_handler (napi_env env, napi_callback_info info);
napi_value socket_subscription_event (napi_env env, napi_callback_info info);
napi_value socket_try_subscription_event (napi_env env, napi_callback_info info);
napi_value subscription_at (napi_env env, napi_callback_info info);
napi_value socket_stream_attach (napi_env env, napi_callback_info info);
napi_value socket_setopt (napi_env env, napi_callback_info info);
napi_value socket_getopt (napi_env env, napi_callback_info info);
napi_value socket_set_subscription (napi_env env, napi_callback_info info);
napi_value socket_unset_subscription (napi_env env, napi_callback_info info);
napi_value handle_set_routing_id (napi_env env, napi_callback_info info);
napi_value handle_get_routing_id (napi_env env, napi_callback_info info);
napi_value dealer_request (napi_env env, napi_callback_info info);
napi_value router_request (napi_env env, napi_callback_info info);
napi_value router_request_transport_pair (napi_env env, napi_callback_info info);
napi_value router_send_transport_pair (napi_env env, napi_callback_info info);
napi_value router_reply (napi_env env, napi_callback_info info);
napi_value router_try_send_completion_control (napi_env env, napi_callback_info info);
napi_value router_try_send_completion_control_transport_pair (napi_env env, napi_callback_info info);
napi_value router_completion_control_handler (napi_env env, napi_callback_info info);
napi_value router_recv_message (napi_env env, napi_callback_info info);
napi_value router_try_recv_message (napi_env env, napi_callback_info info);
napi_value router_try_recv_message_batch (napi_env env, napi_callback_info info);

napi_value monitor_open (napi_env env, napi_callback_info info);
napi_value monitor_handler (napi_env env, napi_callback_info info);
napi_value monitor_recv (napi_env env, napi_callback_info info);
napi_value monitor_try_recv (napi_env env, napi_callback_info info);
napi_value monitor_status (napi_env env, napi_callback_info info);
napi_value monitor_close (napi_env env, napi_callback_info info);

napi_value poller_new (napi_env env, napi_callback_info info);
napi_value poller_destroy (napi_env env, napi_callback_info info);
napi_value poller_size (napi_env env, napi_callback_info info);
napi_value poller_add (napi_env env, napi_callback_info info);
napi_value poller_modify (napi_env env, napi_callback_info info);
napi_value poller_remove (napi_env env, napi_callback_info info);
napi_value poller_add_fd (napi_env env, napi_callback_info info);
napi_value poller_modify_fd (napi_env env, napi_callback_info info);
napi_value poller_remove_fd (napi_env env, napi_callback_info info);
napi_value poller_add_timer (napi_env env, napi_callback_info info);
napi_value poller_remove_timer (napi_env env, napi_callback_info info);
napi_value poller_wait (napi_env env, napi_callback_info info);
napi_value poll_events_new (napi_env env, napi_callback_info info);
napi_value poll_events_destroy (napi_env env, napi_callback_info info);
napi_value poll_events_source_kind (napi_env env, napi_callback_info info);
napi_value poll_events_slot (napi_env env, napi_callback_info info);
napi_value poll_events_revents (napi_env env, napi_callback_info info);
napi_value poll_events_fd (napi_env env, napi_callback_info info);
napi_value poller_wait_into (napi_env env, napi_callback_info info);

napi_value timer_new (napi_env env, napi_callback_info info);
napi_value timer_destroy (napi_env env, napi_callback_info info);
napi_value timer_start (napi_env env, napi_callback_info info);
napi_value timer_stop (napi_env env, napi_callback_info info);
napi_value timer_recv (napi_env env, napi_callback_info info);
napi_value timer_handler (napi_env env, napi_callback_info info);

napi_value stopwatch_start (napi_env env, napi_callback_info info);
napi_value stopwatch_intermediate (napi_env env, napi_callback_info info);
napi_value stopwatch_stop (napi_env env, napi_callback_info info);
napi_value atomic_counter_new (napi_env env, napi_callback_info info);
napi_value atomic_counter_set (napi_env env, napi_callback_info info);
napi_value atomic_counter_inc (napi_env env, napi_callback_info info);
napi_value atomic_counter_dec (napi_env env, napi_callback_info info);
napi_value atomic_counter_value (napi_env env, napi_callback_info info);
napi_value atomic_counter_destroy (napi_env env, napi_callback_info info);
