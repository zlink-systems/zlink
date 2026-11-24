/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/stream_connector_cases.hpp"
#include <string_view>

int main (int argc, char **argv)
{
    using namespace stream_connector_tests;
    if (argc == 2) {
        if (std::string_view (argv[1]) == "login-admission")
            return verify_login_admission ();
        return test_inbound_codec (argv[1]);
    }
    if (const int failure = verify_inbound_framing (); failure != 0)
        return failure;
    if (const int failure = verify_encode_failure (); failure != 0)
        return failure;
    if (const int failure = verify_actor_close_order (); failure != 0)
        return failure;
    if (const int failure = verify_completion_before_suspend (); failure != 0)
        return failure;
    if (const int failure = verify_reply_and_actor_headers (); failure != 0)
        return failure;
    if (const int failure = verify_flow_and_correlation_headers (); failure != 0)
        return failure;
    if (const int failure = verify_default_connector (); failure != 0)
        return failure;
    if (const int failure = verify_write_order (); failure != 0)
        return failure;
    if (const int failure = verify_coroutine_frame_lifetime (); failure != 0)
        return failure;
    if (const int failure = verify_metadata_compression_and_payload_limits (); failure != 0)
        return failure;
    // Each stage calls the next before its local fixtures are destroyed.
    return verify_typed_connection_setup ();
}
