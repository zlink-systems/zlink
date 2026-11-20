/* SPDX-License-Identifier: MPL-2.0 */

#include "contract_socket_pair_fixture.hpp"
// Compile the real private observer here; the static test-core archive then
// supplies the other implementation objects without a production test API.
#include "api/socket/socket_request_reply_submit_api.cpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace zlink
{
class session_termination_test_access_t
{
  public:
    static uint32_t lifetime_refs (pipe_t *pipe_) { return pipe_->_lifetime.refs (); }

    static void flush_observed (pipe_t *pipe_, void *observer_)
    {
        pipe_->flush_unlocked (publish_pending_pair_before_flush, observer_);
    }

    static void resume_reader_without_dispatch (pipe_t *pipe_)
    {
        // Deterministically occupy the observer slot across another sleep/wake
        // cycle without dispatching its first command or changing its ref.
        pipe_->_in_active.store (true, std::memory_order_release);
    }

    static socket_lifecycle_coordinator_t &lifecycle (socket_base_t *socket_)
    {
        return socket_->lifecycle_coordinator ();
    }
};
}

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct delivery_barrier_t
{
    zlink::socket_lifecycle_coordinator_t *coordinator;
    zlink::pipe_t *destination;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool sync_owned = false;
};

void stop_before_mailbox_delivery (const zlink::command_t &command_, void *arg_)
{
    delivery_barrier_t &barrier = *static_cast<delivery_barrier_t *> (arg_);
    if (command_.type != zlink::command_t::activate_read
        || command_.destination != barrier.destination)
        return;
    std::unique_lock<std::mutex> lock (barrier.mutex);
    barrier.sync_owned = barrier.coordinator->public_api_sync_owned_by_current_thread ();
    barrier.entered = true;
    barrier.changed.notify_all ();
    barrier.changed.wait (lock, [&] { return barrier.released; });
}

struct wake_count_t
{
    zlink::pipe_t *destination;
    size_t count = 0;
};

void count_wake_delivery (const zlink::command_t &command_, void *arg_)
{
    wake_count_t &wakes = *static_cast<wake_count_t *> (arg_);
    if (command_.type == zlink::command_t::activate_read
        && command_.destination == wakes.destination)
        ++wakes.count;
}

void write_observed_byte (zlink::pipe_t *writer_,
                          pending_pair_observer_t *observer_,
                          unsigned char value_)
{
    zlink::msg_t message;
    TEST_ASSERT_EQUAL_INT (0, message.init_size (1));
    *static_cast<unsigned char *> (message.data ()) = value_;
    TEST_ASSERT_TRUE (writer_->write (&message));
    zlink::session_termination_test_access_t::flush_observed (writer_, observer_);
}

void read_byte (zlink::pipe_t *reader_, unsigned char expected_)
{
    zlink::msg_t message;
    TEST_ASSERT_EQUAL_INT (0, message.init ());
    TEST_ASSERT_TRUE (reader_->read (&message));
    TEST_ASSERT_EQUAL_UINT64 (1, message.size ());
    TEST_ASSERT_EQUAL_UINT8 (expected_, *static_cast<unsigned char *> (message.data ()));
    TEST_ASSERT_EQUAL_INT (0, message.close ());
}

void test_two_flush_wakes_preserve_delivery_and_refs ()
{
    void *source = test_context_socket (ZLINK_SOCKET_PAIR);
    void *target = test_context_socket (ZLINK_SOCKET_PAIR);
    contract_socket_pair_t pair (source, target);
    zlink::pipe_t *const reader = pair.application[1];
    TEST_ASSERT_FALSE (reader->check_read ());
    const uint32_t refs = zlink::session_termination_test_access_t::lifetime_refs (reader);
    wake_count_t wakes;
    wakes.destination = reader;
    zlink::object_t::test_set_command_delivery_hook (count_wake_delivery, &wakes);
    {
        zlink::socket_public_send_scope_t scope (
          zlink::session_termination_test_access_t::lifecycle (pair.cores[0]), true,
          zlink::socket_send_admission_complete);
        TEST_ASSERT_TRUE (scope.acquired ());
        pending_pair_observer_t observer;
        write_observed_byte (pair.application[0], &observer, 'a');
        TEST_ASSERT_EQUAL_PTR (reader, observer.deferred_command.destination);
        TEST_ASSERT_EQUAL_UINT64 (0, wakes.count);
        TEST_ASSERT_EQUAL_UINT32 (refs + 1,
                                  zlink::session_termination_test_access_t::lifetime_refs (reader));

        // Public admission normally dispatches before the reader can repeat
        // this cycle. Use the existing friend boundary to force an occupied
        // slot for a second real flush in the same scope and observer.
        zlink::session_termination_test_access_t::resume_reader_without_dispatch (reader);
        read_byte (reader, 'a');
        TEST_ASSERT_FALSE (reader->check_read ());
        write_observed_byte (pair.application[0], &observer, 'b');
        TEST_ASSERT_EQUAL_UINT64_MESSAGE (1, wakes.count,
                                          "a refused second wake must be sent immediately");
        TEST_ASSERT_EQUAL_PTR (reader, observer.deferred_command.destination);
        TEST_ASSERT_EQUAL_UINT32 (refs + 2,
                                  zlink::session_termination_test_access_t::lifetime_refs (reader));
        scope.unlock_sync ();
        TEST_ASSERT_TRUE (publish_pending_pair_before_flush (
          NULL, &observer, zlink::pipe_write_observer_dispatch_deferred_command, NULL));
        TEST_ASSERT_NULL (observer.deferred_command.destination);
        TEST_ASSERT_EQUAL_UINT64 (2, wakes.count);
        pair.pump ();
        TEST_ASSERT_EQUAL_UINT32 (refs,
                                  zlink::session_termination_test_access_t::lifetime_refs (reader));
        read_byte (reader, 'b');
        TEST_ASSERT_FALSE (reader->check_read ());
    }
    zlink::object_t::test_set_command_delivery_hook (NULL, NULL);
    TEST_ASSERT_EQUAL_UINT64 (2, wakes.count);
    TEST_ASSERT_EQUAL_UINT32 (refs,
                              zlink::session_termination_test_access_t::lifetime_refs (reader));
    test_context_socket_close_zero_linger (source);
    test_context_socket_close_zero_linger (target);
}

void test_observer_destruction_delivers_undispatched_wake ()
{
    void *source = test_context_socket (ZLINK_SOCKET_PAIR);
    void *target = test_context_socket (ZLINK_SOCKET_PAIR);
    contract_socket_pair_t pair (source, target);
    zlink::pipe_t *const reader = pair.application[1];
    TEST_ASSERT_FALSE (reader->check_read ());
    const uint32_t refs = zlink::session_termination_test_access_t::lifetime_refs (reader);
    wake_count_t wakes;
    wakes.destination = reader;
    zlink::object_t::test_set_command_delivery_hook (count_wake_delivery, &wakes);
    {
        pending_pair_observer_t observer;
        write_observed_byte (pair.application[0], &observer, 'a');
        TEST_ASSERT_EQUAL_PTR (reader, observer.deferred_command.destination);
        TEST_ASSERT_EQUAL_UINT64 (0, wakes.count);
        TEST_ASSERT_EQUAL_UINT32 (refs + 1,
                                  zlink::session_termination_test_access_t::lifetime_refs (reader));
        // Leave without the dispatch phase: the real observer destructor must
        // transfer the published message's wake and its ref to the mailbox.
    }
    zlink::object_t::test_set_command_delivery_hook (NULL, NULL);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE (1, wakes.count,
                                      "observer destruction must deliver the pending wake");
    TEST_ASSERT_EQUAL_UINT32 (refs + 1,
                              zlink::session_termination_test_access_t::lifetime_refs (reader));
    pair.pump ();
    TEST_ASSERT_EQUAL_UINT32 (refs,
                              zlink::session_termination_test_access_t::lifetime_refs (reader));
    read_byte (reader, 'a');
    TEST_ASSERT_FALSE (reader->check_read ());
    test_context_socket_close_zero_linger (source);
    test_context_socket_close_zero_linger (target);
}

void test_delivery_probe_accepts_unlocked_writer ()
{
    void *source = test_context_socket (ZLINK_SOCKET_PAIR);
    void *target = test_context_socket (ZLINK_SOCKET_PAIR);
    contract_socket_pair_t pair (source, target);
    TEST_ASSERT_FALSE (pair.application[1]->check_read ());
    delivery_barrier_t barrier;
    barrier.coordinator = &zlink::session_termination_test_access_t::lifecycle (pair.cores[0]);
    barrier.destination = pair.application[1];
    barrier.released = true;
    zlink::msg_t message;
    TEST_ASSERT_EQUAL_INT (0, message.init_size (1));
    zlink::object_t::test_set_command_delivery_hook (stop_before_mailbox_delivery, &barrier);
    const bool written = pair.application[0]->write (&message);
    pair.application[0]->flush ();
    zlink::object_t::test_set_command_delivery_hook (NULL, NULL);
    if (!written)
        message.close ();
    pair.pump ();
    const bool observed = barrier.entered;
    const bool sync_owned = barrier.sync_owned;
    test_context_socket_close_zero_linger (source);
    test_context_socket_close_zero_linger (target);
    TEST_ASSERT_TRUE (written);
    TEST_ASSERT_TRUE (observed);
    TEST_ASSERT_FALSE (sync_owned);
}

void test_request_wake_delivery_releases_public_sync ()
{
    void *source = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *target = test_context_socket (ZLINK_SOCKET_ROUTER);
    contract_socket_pair_t pair (source, target);
    // Fix the sleeping-reader order before starting the producer.
    TEST_ASSERT_FALSE (pair.application[1]->check_read ());
    delivery_barrier_t barrier;
    barrier.coordinator = &zlink::session_termination_test_access_t::lifecycle (pair.cores[0]);
    barrier.destination = pair.application[1];
    zlink_submit_result_t result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_completion_id_t completion_id = 0;
    std::thread submitter ([&] {
        zlink::object_t::test_set_command_delivery_hook (stop_before_mailbox_delivery, &barrier);
        zlink_msg_t parts[2];
        zlink_msg_init_size (&parts[0], 76);
        zlink_msg_init_size (&parts[1], 1024);
        memset (zlink_msg_data (&parts[0]), 'h', 76);
        memset (zlink_msg_data (&parts[1]), 'p', 1024);
        result = zlink_request (source, &pair.rids[1], parts, 2, ZLINK_SEND_FLAGS_NONE, 10000, NULL,
                                &completion_id);
        zlink_multipart_close (parts, 2);
        zlink::object_t::test_set_command_delivery_hook (NULL, NULL);
    });
    bool entered;
    bool sync_owned;
    {
        std::unique_lock<std::mutex> lock (barrier.mutex);
        entered = barrier.changed.wait_for (lock, std::chrono::seconds (5),
                                            [&] { return barrier.entered; });
        sync_owned = barrier.sync_owned;
    }
    // The bound is only hang cleanup. Lock ownership decides the result.
    bool same_socket_entered = false;
    if (entered && !sync_owned) {
        zlink::socket_public_send_scope_t scope (*barrier.coordinator, true,
                                                 zlink::socket_send_admission_complete);
        same_socket_entered = scope.acquired ();
    }
    {
        std::lock_guard<std::mutex> lock (barrier.mutex);
        barrier.released = true;
        barrier.changed.notify_all ();
    }
    submitter.join ();
    pair.pump ();
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t token = 0;
    zlink_msg_t received[2];
    size_t count = 0;
    const zlink_recv_result_t received_result =
      zlink_router_recv (target, &rid, &token, received, 2, &count, ZLINK_RECV_FLAGS_DONTWAIT);
    if (received_result == ZLINK_RECV_OK)
        zlink_multipart_close (received, count);
    test_context_socket_close_zero_linger (source);
    test_context_socket_close_zero_linger (target);
    TEST_ASSERT_TRUE (entered);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, received_result);
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    TEST_ASSERT_FALSE_MESSAGE (sync_owned,
                               "wake delivery must execute after releasing public API sync");
    TEST_ASSERT_TRUE (same_socket_entered);
}
}

int main (int argc, char **argv)
{
    const char *selected = NULL;
    if (argc == 3 && strcmp (argv[1], "-n") == 0)
        selected = argv[2];
    else if (argc != 1)
        return 1;
    UNITY_BEGIN ();
#define RUN_SELECTED(test_)                                                                        \
    if (!selected || strcmp (selected, #test_) == 0)                                               \
    RUN_TEST (test_)
    RUN_SELECTED (test_delivery_probe_accepts_unlocked_writer);
    RUN_SELECTED (test_request_wake_delivery_releases_public_sync);
    RUN_SELECTED (test_two_flush_wakes_preserve_delivery_and_refs);
    RUN_SELECTED (test_observer_destruction_delivers_undispatched_wake);
#undef RUN_SELECTED
    const bool matched = !selected || Unity.NumberOfTests == 1;
    const int result = UNITY_END ();
    return matched ? result : 1;
}
