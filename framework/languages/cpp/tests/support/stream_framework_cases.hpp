/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime_metric_contract.hpp"

namespace zlink::framework::test::stream_framework
{
enum class connection_close_case_t
{
    connected_rejected,
    client_close,
    disconnect_rejected,
    server_stop,
    heartbeat_timeout
};

#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
enum class core_lifecycle_case_t
{
    normal,
    drain,
    factory_failure
};

int core_session_lifecycle_boundaries (core_lifecycle_case_t scenario);
#endif

int core_disconnect_reason_contract (metric_test::provider_t &metrics);

int expired_core_disconnect_not_found_does_not_report_or_stop_next_session ();

int core_connected_rejection_cleanup (
  metric_test::provider_t &metrics,
  connection_close_case_t close_case = connection_close_case_t::connected_rejected);

int descriptor_unavailable_replacement_records_drop ();

int reader_retirement_is_tracked_until_complete ();

#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
int transport_bound_close (bool tls, bool remote = false);
#endif

int binding_submission_order ();

int reply_submission_after_unbound (bool retire = true);

int stream_write_terminal_during_host_shutdown (bool success);

int session_creation_failure (bool websocket);

int stream_dispatch_cases (metric_test::provider_t &metric_provider);
int stream_listener_cases (metric_test::provider_t &metric_provider);
int stream_core_shutdown_cases (metric_test::provider_t &metric_provider);
} // namespace zlink::framework::test::stream_framework
