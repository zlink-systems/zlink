/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "sockets/common/socket_base.hpp"

#include <chrono>
#include <future>
#include <thread>

void setUp ()
{
}

void tearDown ()
{
}

namespace zlink
{
class session_termination_test_access_t
{
  public:
    static bool close_accepted (const socket_base_t *socket_)
    {
        return socket_->lifecycle_coordinator ().public_close_requested ();
    }
};
}

namespace
{
struct command_gate_t
{
    std::promise<void> entered;
    std::promise<void> release;
};

void pause_readiness_command (void *arg_, int command_, bool, bool)
{
    if (command_ != zlink::command_t::request_completion)
        return;

    command_gate_t &gate = *static_cast<command_gate_t *> (arg_);
    gate.entered.set_value ();
    gate.release.get_future ().wait ();
}

void test_close_during_poller_observation ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *handle = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (handle);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add (poller, handle, NULL, ZLINK_POLLIN));
    zlink::socket_base_t *const socket = as_socket_handle (handle).socket;

    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    int count = -1;
    bool command_entered = false;
    bool close_accepted = false;
    zlink_close_result_t close_rc;
    {
        command_gate_t gate;
        std::future<void> entered = gate.entered.get_future ();
        socket->test_set_receive_command_sync_probe_hook (&pause_readiness_command, &gate);
        socket->notify_request_completion ();

        std::thread waiter ([&] { count = zlink_poller_wait (poller, &event, 1, 1000, &error); });

        command_entered = entered.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
        std::promise<zlink_close_result_t> result;
        std::future<zlink_close_result_t> closed = result.get_future ();
        std::thread closer ([&] { result.set_value (zlink_close (handle)); });

        // The closer may join the socket turn after accepting close. Resume the
        // poller once the coordinator accepts close or the public call rejects it.
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
        do {
            close_accepted = zlink::session_termination_test_access_t::close_accepted (socket);
            if (close_accepted
                || closed.wait_for (std::chrono::milliseconds (0)) == std::future_status::ready)
                break;
            std::this_thread::yield ();
        } while (std::chrono::steady_clock::now () < deadline);

        gate.release.set_value ();
        closer.join ();
        waiter.join ();
        socket->test_set_receive_command_sync_probe_hook (NULL, NULL);
        close_rc = closed.get ();
    }

    zlink_poller_event_t next_event = {};
    zlink_config_result_t next_error = ZLINK_CONFIG_INTERNAL_ERROR;
    const int next_count = zlink_poller_wait (poller, &next_event, 1, 0, &next_error);

    const zlink_close_result_t poller_close_rc = zlink_poller_destroy (&poller);
    if (close_rc == ZLINK_CLOSE_BUSY)
        (void) zlink_close (handle);
    const zlink_close_result_t ctx_close_rc = zlink_ctx_term (ctx);

    TEST_ASSERT_TRUE (command_entered);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, close_rc);
    TEST_ASSERT_TRUE (close_accepted);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (1, count);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLERR, event.events);
    TEST_ASSERT_EQUAL_PTR (handle, event.socket);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, next_error);
    TEST_ASSERT_EQUAL_INT (0, next_count);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, poller_close_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, ctx_close_rc);
}
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_close_during_poller_observation);
    return UNITY_END ();
}
