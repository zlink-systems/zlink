/* SPDX-License-Identifier: MPL-2.0 */

//  Regression coverage for zlink_send_async admission over a handshaking
//  transport.
//
//  On tcp the very first async record is admitted inline from the submitting
//  thread, so a missing writable wake is invisible. On tls the connection is
//  not usable until the handshake finishes, so the record parks in the pending
//  queue and only a writable transition can admit it. If that transition never
//  reaches drive_send_pending(), the completion never fires and every binding
//  that suspends on it (Java CompletionStage, Node Promise) hangs forever.

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "testutil_send_complete.hpp"

#include <cstring>
#include <string>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const size_t payload_size = 1024;
const int completion_wait_ms = 10000;

bool tls_available ()
{
    return zlink_has ("tls") != 0;
}

void configure_tls (void *server_, void *client_, const tls_test_files_t &files_)
{
    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_CERT, files_.server_cert.c_str (),
      files_.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_KEY, files_.server_key.c_str (),
      files_.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_CA, files_.ca_cert.c_str (), files_.ca_cert.size ()));
    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));
}

//  Submits one 1024B record on a DEALER. Immediate admission returns op_id zero
//  without a callback; a handshaking/HWM-pending record completes by callback.
//  `settle_ms_` selects the two variants: 0 submits as early as the socket will
//  accept a target (handshake still in flight), a positive value submits after
//  the connection has settled.
void run_case (const char *transport_, int settle_ms_)
{
    if (strcmp (transport_, "tls") == 0 && !tls_available ())
        TEST_IGNORE_MESSAGE ("tls transport not available");

    void *server = zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    void *client = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (client);

    tls_test_files_t tls_files;
    const bool is_tls = strcmp (transport_, "tls") == 0;
    if (is_tls) {
        tls_files = make_tls_test_files ();
        configure_tls (server, client, tls_files);
    }

    char endpoint[MAX_SOCKET_STRING];
    if (is_tls)
        test_bind (server, "tls://127.0.0.1:*", endpoint, sizeof (endpoint));
    else
        test_bind (server, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));

    zlink_test::send_complete_probe_t probe;
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_OK,
                           zlink_send_complete_handler (
                             client, &zlink_test::record_send_complete, &probe));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    if (settle_ms_ > 0)
        msleep (settle_ms_);

    std::string payload (payload_size, 'x');
    zlink_send_async_options_t options = zlink_test::make_send_async_options ();
    zlink_send_op_id_t op_id = 0;

    //  A DEALER can only resolve a target once a peer pipe exists. Retry until
    //  the connection produces one; that retry is about target resolution, not
    //  about admission - once the submit is accepted the record is Core's and
    //  the completion must arrive on its own.
    zlink_submit_result_t submit = ZLINK_SUBMIT_INTERNAL_ERROR;
    for (int i = 0; i != completion_wait_ms / 5; ++i) {
        submit = zlink_test::send_async_bytes (client, payload.data (),
                                               payload.size (), &options, &op_id);
        if (submit == ZLINK_SUBMIT_OK)
            break;
        msleep (5);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit);
    if (op_id == 0) {
        TEST_ASSERT_EQUAL_INT (
          0, probe.count.load (std::memory_order_acquire));
    } else {
        //  The socket owns the pending record now. Nothing below touches the
        //  socket, so admission must be driven by the transport wake itself.
        int waited = 0;
        while (probe.count.load (std::memory_order_acquire) == 0
               && waited < completion_wait_ms) {
            msleep (10);
            waited += 10;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE (
          1, probe.count.load (std::memory_order_acquire),
          "send_async completion never fired: pending record was never admitted");
        TEST_ASSERT_EQUAL_INT (
          1, probe.admitted.load (std::memory_order_acquire));
        TEST_ASSERT_EQUAL_UINT64 (op_id, probe.events[0].op_id);
    }

    //  The bytes really reached the peer, so this is admission and not a
    //  completion invented by the wake path.
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const zlink_recv_result_t rc = zlink_router_recv_part (
      server, &source, &request_seq, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
    TEST_ASSERT_EQUAL_UINT (payload_size, zlink_msg_size (&part));
    zlink_msg_close (&part);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (client));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (server));
    if (is_tls)
        cleanup_tls_test_files (tls_files);
}
}

void test_send_async_admits_over_tcp ()
{
    run_case ("tcp", SETTLE_TIME);
}

void test_send_async_admits_over_tls ()
{
    run_case ("tls", SETTLE_TIME);
}

//  The interesting variant: the record is submitted while the TLS handshake is
//  still in flight, so admission can only come from the writable transition.
void test_send_async_admits_over_tls_during_handshake ()
{
    run_case ("tls", 0);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_send_async_admits_over_tcp);
    RUN_TEST (test_send_async_admits_over_tls);
    RUN_TEST (test_send_async_admits_over_tls_during_handshake);
    return UNITY_END ();
}
