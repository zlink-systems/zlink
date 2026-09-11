/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int wait_ms = 5000;
const size_t filler_size = 65536;
const size_t max_fill_attempts = 512;
char case_failures[4][512];

bool selected (const char *name_)
{
    const char *const value = getenv ("ZLINK_TEST_CASE");
    return !value || !*value || strcmp (value, name_) == 0;
}

struct submission_t
{
    submission_t () : result (ZLINK_SUBMIT_INTERNAL_ERROR), error (0),
                      completion_id (0), remaining_size (0), close_result (0),
                      initialized (false)
    {
    }

    zlink_submit_result_t result;
    int error;
    zlink_completion_id_t completion_id;
    size_t remaining_size;
    int close_result;
    bool initialized;
};

struct received_record_t
{
    received_record_t () : result (ZLINK_RECV_INTERNAL_ERROR), error (0),
                           token (0), close_result (0)
    {
        memset (&source, 0, sizeof (source));
    }

    zlink_recv_result_t result;
    int error;
    zlink_routing_id_t source;
    zlink_reply_token_t token;
    std::vector<std::string> parts;
    int close_result;
};

struct completion_result_t
{
    completion_result_t () : result (ZLINK_RECV_INTERNAL_ERROR), error (0),
                             kind (ZLINK_COMPLETION_SEND), completion_id (0),
                             user_context (NULL),
                             send_result (ZLINK_SEND_ADMITTED),
                             send_terminal_errno (0),
                             request_result (ZLINK_REQUEST_OK), close_result (0)
    {
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
    }

    zlink_recv_result_t result;
    int error;
    zlink_completion_t completion;
    zlink_completion_kind_t kind;
    zlink_completion_id_t completion_id;
    void *user_context;
    zlink_send_complete_result_t send_result;
    int send_terminal_errno;
    zlink_request_result_t request_result;
    std::vector<std::string> reply_parts;
    int close_result;
};

struct case_sync_t
{
    case_sync_t () : a_ready (false), release_a (false),
                     a_submitted (false), b_retried (false),
                     b_received (false), a_received (false), abort (false)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool a_ready;
    bool release_a;
    bool a_submitted;
    bool b_retried;
    bool b_received;
    bool a_received;
    bool abort;

    std::mutex failure_mutex;
    std::string failure;
};

void fail (case_sync_t *sync_, const std::string &message_)
{
    std::lock_guard<std::mutex> lock (sync_->failure_mutex);
    if (sync_->failure.empty ())
        sync_->failure = message_;
}

void abort_case (case_sync_t *sync_, const std::string &message_)
{
    fail (sync_, message_);
    {
        std::lock_guard<std::mutex> lock (sync_->mutex);
        sync_->abort = true;
    }
    sync_->changed.notify_all ();
}

bool is_aborted (case_sync_t *sync_)
{
    std::lock_guard<std::mutex> lock (sync_->mutex);
    return sync_->abort;
}

bool has_failure (case_sync_t *sync_)
{
    std::lock_guard<std::mutex> lock (sync_->failure_mutex);
    return !sync_->failure.empty ();
}

template <typename Predicate>
bool wait_for_state (case_sync_t *sync_, Predicate predicate_)
{
    std::unique_lock<std::mutex> lock (sync_->mutex);
    return sync_->changed.wait_for (
      lock, std::chrono::milliseconds (wait_ms), predicate_);
}

submission_t submit_record (void *dealer_, bool request_,
                            const std::vector<std::string> &payloads_,
                            void *context_)
{
    submission_t submitted;
    std::vector<zlink_msg_t> parts (payloads_.size ());
    for (size_t i = 0; i < parts.size (); ++i) {
        if (zlink_msg_init_size (&parts[i], payloads_[i].size ()) != ZLINK_CONFIG_OK) {
            submitted.error = zlink_errno ();
            zlink_multipart_close (parts.data (), i);
            return submitted;
        }
        if (!payloads_[i].empty ())
            memcpy (zlink_msg_data (&parts[i]), payloads_[i].data (), payloads_[i].size ());
    }
    submitted.initialized = true;
    errno = 0;
    if (request_)
        submitted.result = zlink_request (
          dealer_, NULL, parts.data (), parts.size (), ZLINK_SEND_FLAGS_DONTWAIT,
          30000, context_, &submitted.completion_id);
    else
        submitted.result = zlink_send (
          dealer_, parts.data (), parts.size (), ZLINK_SEND_FLAGS_DONTWAIT,
          context_, &submitted.completion_id);
    submitted.error = zlink_errno ();
    for (size_t i = 0; i < parts.size (); ++i) {
        submitted.remaining_size += zlink_msg_size (&parts[i]);
        const int close_rc = zlink_msg_close (&parts[i]);
        if (close_rc != ZLINK_CONFIG_OK)
            submitted.close_result = close_rc;
    }
    return submitted;
}

received_record_t receive_record (void *router_)
{
    received_record_t received;
    const zlink_routing_id_t *source = NULL;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    errno = 0;
    received.result = zlink_router_recv (
      router_, &source, &received.token, &parts, &part_count,
      ZLINK_RECV_FLAGS_NONE);
    received.error = zlink_errno ();
    if (received.result != ZLINK_RECV_OK)
        return received;
    if (source)
        received.source = *source;
    for (size_t i = 0; i != part_count; ++i)
        received.parts.push_back (std::string (
          static_cast<const char *> (zlink_msg_data (&parts[i])),
          zlink_msg_size (&parts[i])));
    zlink_multipart_close (parts, part_count);
    received.close_result = ZLINK_CONFIG_OK;
    return received;
}

submission_t reply_to (void *router_, const received_record_t &request_,
                       const char *payload_)
{
    submission_t submitted;
    zlink_msg_t part;
    const size_t size = strlen (payload_);
    if (zlink_msg_init_size (&part, size) != ZLINK_CONFIG_OK) {
        submitted.error = zlink_errno ();
        return submitted;
    }
    submitted.initialized = true;
    memcpy (zlink_msg_data (&part), payload_, size);
    errno = 0;
    submitted.result = zlink_reply (router_, &request_.source, request_.token, &part, 1);
    submitted.error = zlink_errno ();
    submitted.remaining_size = zlink_msg_size (&part);
    submitted.close_result = zlink_msg_close (&part);
    return submitted;
}

completion_result_t receive_completion (void *dealer_)
{
    completion_result_t received;
    errno = 0;
    received.result = zlink_completion_recv (
      dealer_, &received.completion, ZLINK_RECV_FLAGS_NONE);
    received.error = zlink_errno ();
    if (received.result == ZLINK_RECV_OK) {
        received.kind = received.completion.kind;
        received.completion_id = received.completion.completion_id;
        received.user_context = received.completion.user_context;
        received.send_result = received.completion.send_result;
        received.send_terminal_errno =
          received.completion.send_terminal_errno;
        received.request_result = received.completion.request_result;
        for (size_t i = 0; i != received.completion.reply_part_count; ++i)
            received.reply_parts.push_back (std::string (
              static_cast<const char *> (zlink_msg_data (
                &received.completion.reply_parts[i])),
              zlink_msg_size (&received.completion.reply_parts[i])));
        zlink_completion_close (&received.completion);
        received.close_result = ZLINK_CONFIG_OK;
    }
    return received;
}

bool submission_consumed (const submission_t &submitted_)
{
    return submitted_.initialized && submitted_.remaining_size == 0
           && submitted_.close_result == ZLINK_CONFIG_OK;
}

bool record_matches (const received_record_t &record_, const char *first_,
                     const char *second_, bool request_)
{
    return record_.result == ZLINK_RECV_OK
           && record_.close_result == ZLINK_CONFIG_OK
           && record_.parts.size () == 2 && record_.parts[0] == first_
           && record_.parts[1] == second_
           && (request_ ? record_.token != 0 : record_.token == 0);
}

bool completion_matches_request (const completion_result_t &received_,
                                 zlink_completion_id_t id_, void *context_,
                                 const char *reply_)
{
    return received_.result == ZLINK_RECV_OK
           && received_.close_result == ZLINK_CONFIG_OK
           && received_.kind == ZLINK_COMPLETION_REQUEST
           && received_.completion_id == id_
           && received_.user_context == context_
           && received_.request_result == ZLINK_REQUEST_OK
           && received_.reply_parts.size () == 1
           && received_.reply_parts[0] == reply_;
}

void configure_socket (void *socket_, const char *routing_id_)
{
    // Fillers drive the pipe to a measured DONTWAIT rejection.  The HWM still
    // has room for the small two-part B record after credit recovery.
    const uint64_t hwm = 128u * 1024u;
    const int linger = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &wait_ms,
                        sizeof (wait_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &wait_ms,
                        sizeof (wait_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (socket_, routing_id_, strlen (routing_id_)));
}

void *open_ready_monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECTION_READY
                     | ZLINK_EVENT_SEND_FLOW_PAUSED
                     | ZLINK_EVENT_SEND_FLOW_RESUMED;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    return monitor;
}

void wait_ready (void *monitor_)
{
    zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poll (&item, 1, wait_ms, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, item.revents);
    zlink_socket_monitor_event_t event;
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_socket_monitor_recv (monitor_, &event, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_CONNECTION_READY, event.event);
}

void wait_monitor_event (void *monitor_, uint64_t expected_)
{
    zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poll (&item, 1, wait_ms, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, item.revents);
    zlink_socket_monitor_event_t event;
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_socket_monitor_recv (monitor_, &event, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (expected_, event.event);
}

void describe_submission_failure (case_sync_t *sync_, const char *label_,
                                  const submission_t &submitted_)
{
    std::ostringstream message;
    message << label_ << " result=" << submitted_.result
            << " errno=" << submitted_.error
            << " id=" << submitted_.completion_id
            << " remaining=" << submitted_.remaining_size
            << " close=" << submitted_.close_result;
    fail (sync_, message.str ());
}

void describe_completion_failure (case_sync_t *sync_, const char *label_,
                                  const completion_result_t &received_,
                                  zlink_completion_id_t expected_id_,
                                  void *expected_context_)
{
    std::ostringstream message;
    message << label_ << " recv=" << received_.result
            << " errno=" << received_.error << " kind=" << received_.kind
            << " id=" << received_.completion_id
            << " expected_id=" << expected_id_
            << " context=" << received_.user_context
            << " expected_context=" << expected_context_
            << " request_result=" << received_.request_result
            << " reply_parts=" << received_.reply_parts.size ();
    fail (sync_, message.str ());
}

bool run_case (const char *transport_, bool request_, size_t serial_,
               char *failure_out_, size_t failure_capacity_)
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    configure_socket (router, "whole-record-router");
    configure_socket (dealer, "whole-record-dealer");
    void *monitor = open_ready_monitor (dealer);

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    if (strcmp (transport_, "inproc") == 0) {
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://writable-resubmit-whole-record-%u",
                  static_cast<unsigned> (serial_));
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    } else {
        TEST_ASSERT_EQUAL_STRING ("tcp", transport_);
        test_bind (router, "tcp://127.0.0.1:*", endpoint,
                   sizeof (endpoint));
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, endpoint));
    wait_ready (monitor);

    // Establish the application lane before changing its absolute receive-flow
    // state. READY alone does not require the peer to have consumed a record.
    const submission_t prime =
      submit_record (dealer, false, {"flow-prime"}, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, prime.result);
    TEST_ASSERT_TRUE (submission_consumed (prime));
    const received_record_t primed = receive_record (router);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, primed.result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, primed.close_result);
    TEST_ASSERT_EQUAL_UINT64 (0, primed.token);
    TEST_ASSERT_EQUAL_UINT64 (1, primed.parts.size ());
    TEST_ASSERT_EQUAL_STRING ("flow-prime", primed.parts[0].c_str ());

    const char *const a_first = request_ ? "request-a-first" : "send-a-first";
    const char *const a_final = request_ ? "request-a-final" : "send-a-final";
    // Use sizable records as the HWM regression payload while an explicit
    // PAUSED flow state supplies the deterministic TCP rejection boundary.
    const std::string b_first_payload (
      120u * 1024u, request_ ? 'b' : 'B');
    const std::string b_final_payload (
      4096u, request_ ? 'c' : 'C');
    const char *const b_first = b_first_payload.c_str ();
    const char *const b_final = b_final_payload.c_str ();
    int contexts[4] = {100, 101, 102, 103};
    const std::string filler (filler_size, 'f');
    size_t accepted_fillers = 0;
    submission_t filler_backpressure;
    void *poller = NULL;
    case_sync_t sync;
    submission_t a_submit;

    std::thread application_a ([&] {
        {
            std::lock_guard<std::mutex> lock (sync.mutex);
            sync.a_ready = true;
        }
        sync.changed.notify_all ();
        std::unique_lock<std::mutex> lock (sync.mutex);
        sync.changed.wait (
          lock, [&] { return sync.release_a || sync.abort; });
        if (sync.abort)
            return;
        lock.unlock ();

        a_submit = submit_record (
          dealer, request_, {a_first, a_final},
          request_ ? static_cast<void *> (&contexts[3]) : NULL);
        {
            std::lock_guard<std::mutex> done_lock (sync.mutex);
            sync.a_submitted = true;
        }
        sync.changed.notify_all ();
        if (a_submit.result != ZLINK_SUBMIT_OK
            || !submission_consumed (a_submit)
            || (request_ ? a_submit.completion_id == 0
                         : a_submit.completion_id != 0))
            describe_submission_failure (&sync, "A record", a_submit);
    });

    bool a_ready = wait_for_state (
      &sync, [&] { return sync.a_ready || sync.abort; });
    if (!a_ready)
        abort_case (&sync, "timed out waiting for submitter A");
    if (has_failure (&sync))
        abort_case (&sync, "submitter A failed");

    // Keep A waiting while B is rejected and resubmitted from another thread.
    // Both submissions own complete records and independent completion IDs.
    if (!is_aborted (&sync)) {
        poller = zlink_poller_new ();
        if (!poller
            || zlink_poller_add (
                 poller, dealer, NULL, ZLINK_POLLCOMPLETION)
                 != ZLINK_CONFIG_OK)
            abort_case (&sync, "could not register completion poller");
    }
    if (!is_aborted (&sync)) {
        // Hold a public, observable flow boundary while both rejected records
        // are submitted. This makes TCP rejection independent of writer timing.
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_socket_set_receive_flow_state (
            router, ZLINK_RECEIVE_FLOW_PAUSED));
        wait_monitor_event (monitor, ZLINK_EVENT_SEND_FLOW_PAUSED);
    }
    if (!is_aborted (&sync)) {
        size_t attempts = 0;
        for (; attempts != max_fill_attempts; ++attempts) {
            submission_t submitted = submit_record (
              dealer, false, {filler}, &contexts[0]);
            if (!submission_consumed (submitted)) {
                describe_submission_failure (&sync, "filler", submitted);
                break;
            }
            if (submitted.result == ZLINK_SUBMIT_OK) {
                if (submitted.completion_id != 0) {
                    describe_submission_failure (&sync, "filler", submitted);
                    break;
                }
                ++accepted_fillers;
                continue;
            }
            if (submitted.result != ZLINK_SUBMIT_BACKPRESSURED
                || submitted.error != EAGAIN
                || submitted.completion_id == 0) {
                describe_submission_failure (&sync, "filler", submitted);
                break;
            }

            // Observe the token at the completion boundary without adding an
            // arbitrary quiet-time window. The receiver remains observably
            // PAUSED until B has also crossed the same rejection boundary.
            zlink_poller_event_t event;
            memset (&event, 0, sizeof (event));
            zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
            const int count =
              zlink_poller_wait (poller, &event, 1, 0, &poll_error);
            if (count == 0 && poll_error == ZLINK_CONFIG_OK) {
                filler_backpressure = submitted;
                break;
            }
            if (count != 1 || poll_error != ZLINK_CONFIG_OK
                || (event.events & ZLINK_POLLCOMPLETION) == 0) {
                abort_case (&sync, "filler stability poll failed");
                break;
            }
            completion_result_t transient = receive_completion (dealer);
            if (transient.result != ZLINK_RECV_OK
                || transient.kind != ZLINK_COMPLETION_WRITABLE
                || transient.completion_id != submitted.completion_id
                || transient.user_context != &contexts[0]
                || transient.send_result != ZLINK_SEND_ADMITTED
                || transient.send_terminal_errno != 0) {
                abort_case (&sync, "transient filler WRITABLE mismatch");
                break;
            }
        }
        if (attempts == max_fill_attempts
            || filler_backpressure.result != ZLINK_SUBMIT_BACKPRESSURED
            || filler_backpressure.error != EAGAIN
            || filler_backpressure.completion_id == 0)
            describe_submission_failure (
              &sync, "filler did not establish backpressure",
              filler_backpressure);
        if (has_failure (&sync))
            abort_case (&sync, "filler setup failed");
    }

    submission_t b_initial;
    if (!is_aborted (&sync)) {
        std::thread initial_submitter ([&] {
            b_initial = submit_record (
              dealer, request_, {b_first, b_final}, &contexts[1]);
        });
        initial_submitter.join ();
        if (b_initial.result != ZLINK_SUBMIT_BACKPRESSURED
            || b_initial.error != EAGAIN
            || b_initial.completion_id == 0
            || !submission_consumed (b_initial))
            describe_submission_failure (&sync, "B initial record",
                                         b_initial);
    }
    if (has_failure (&sync))
        abort_case (&sync, "initial B submission failed");

    if (!is_aborted (&sync)) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_socket_set_receive_flow_state (
            router, ZLINK_RECEIVE_FLOW_RUNNING));
        wait_monitor_event (monitor, ZLINK_EVENT_SEND_FLOW_RESUMED);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));

    received_record_t filler_record;
    received_record_t b_record;
    received_record_t a_record;
    submission_t b_reply;
    submission_t a_reply;
    std::thread receiver;
    if (!is_aborted (&sync)) {
        receiver = std::thread ([&] {
            for (size_t i = 0; i != accepted_fillers; ++i) {
                filler_record = receive_record (router);
                if (filler_record.result != ZLINK_RECV_OK
                    || filler_record.close_result != ZLINK_CONFIG_OK
                    || filler_record.parts.size () != 1
                    || filler_record.parts[0] != filler
                    || filler_record.token != 0) {
                    abort_case (
                      &sync, "receiver did not drain every intact filler");
                    return;
                }
            }

            std::unique_lock<std::mutex> lock (sync.mutex);
            sync.changed.wait (
              lock, [&] { return sync.b_retried || sync.abort; });
            if (sync.abort || !sync.b_retried)
                return;
            lock.unlock ();

            b_record = receive_record (router);
            if (!record_matches (b_record, b_first, b_final, request_)) {
                abort_case (&sync,
                            "receiver did not get B as one intact 2-part record");
                return;
            }
            if (request_) {
                b_reply = reply_to (router, b_record, "reply-b");
                if (b_reply.result != ZLINK_SUBMIT_OK
                    || !submission_consumed (b_reply)) {
                    abort_case (&sync, "receiver could not reply to B");
                    return;
                }
            }
            {
                std::lock_guard<std::mutex> received_lock (sync.mutex);
                sync.b_received = true;
            }
            sync.changed.notify_all ();

            lock.lock ();
            sync.changed.wait (
              lock, [&] { return sync.a_submitted || sync.abort; });
            if (sync.abort || !sync.a_submitted)
                return;
            lock.unlock ();

            a_record = receive_record (router);
            if (!record_matches (a_record, a_first, a_final, request_)) {
                abort_case (&sync,
                            "receiver did not get A as one intact 2-part record");
                return;
            }
            if (request_) {
                a_reply = reply_to (router, a_record, "reply-a");
                if (a_reply.result != ZLINK_SUBMIT_OK
                    || !submission_consumed (a_reply)) {
                    abort_case (&sync, "receiver could not reply to A");
                    return;
                }
            }
            {
                std::lock_guard<std::mutex> received_lock (sync.mutex);
                sync.a_received = true;
            }
            sync.changed.notify_all ();
        });
    }

    completion_result_t writable;
    submission_t b_retry;
    completion_result_t b_completion;
    completion_result_t a_completion;

    if (!is_aborted (&sync)) {
        bool saw_filler = false;
        bool saw_b = false;
        for (size_t i = 0; i != 2 && !is_aborted (&sync); ++i) {
            zlink_poller_event_t event;
            memset (&event, 0, sizeof (event));
            zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
            const int count =
              zlink_poller_wait (poller, &event, 1, wait_ms, &poll_error);
            if (count != 1 || poll_error != ZLINK_CONFIG_OK
                || (event.events & ZLINK_POLLCOMPLETION) == 0) {
                abort_case (&sync, "WRITABLE poller did not wake");
                break;
            }
            completion_result_t candidate = receive_completion (dealer);
            if (candidate.result != ZLINK_RECV_OK
                || candidate.close_result != ZLINK_CONFIG_OK
                || candidate.kind != ZLINK_COMPLETION_WRITABLE
                || candidate.send_result != ZLINK_SEND_ADMITTED
                || candidate.send_terminal_errno != 0) {
                abort_case (&sync, "completion owner received invalid WRITABLE");
                break;
            }
            if (candidate.completion_id
                == filler_backpressure.completion_id) {
                if (candidate.user_context != &contexts[0] || saw_filler) {
                    abort_case (&sync, "filler WRITABLE identity mismatch");
                    break;
                }
                saw_filler = true;
            } else if (candidate.completion_id
                       == b_initial.completion_id) {
                if (candidate.user_context != &contexts[1] || saw_b) {
                    abort_case (&sync, "B WRITABLE identity mismatch");
                    break;
                }
                writable = candidate;
                saw_b = true;
            } else {
                abort_case (&sync, "completion owner received unknown WRITABLE");
                break;
            }
        }
        if (!is_aborted (&sync) && (!saw_filler || !saw_b))
            abort_case (&sync, "completion owner did not drain both WRITABLE tokens");
    }
    if (!is_aborted (&sync)) {
        std::thread retry_submitter ([&] {
            b_retry = submit_record (
              dealer, request_, {b_first, b_final}, &contexts[2]);
        });
        retry_submitter.join ();
        if (b_retry.result != ZLINK_SUBMIT_OK
            || !submission_consumed (b_retry)
            || (request_ ? b_retry.completion_id == 0
                         : b_retry.completion_id != 0))
            describe_submission_failure (&sync, "B retry record", b_retry);
        if (has_failure (&sync))
            abort_case (&sync, "B retry submission failed");
        else {
            std::lock_guard<std::mutex> lock (sync.mutex);
            sync.b_retried = true;
            sync.changed.notify_all ();
        }
    }

    if (!is_aborted (&sync)) {
        const bool received_b = wait_for_state (
          &sync, [&] { return sync.b_received || sync.abort; });
        if (!received_b || !sync.b_received)
            abort_case (&sync, "timed out waiting for intact B record");
    }
    if (!is_aborted (&sync) && request_) {
        b_completion = receive_completion (dealer);
        if (!completion_matches_request (
              b_completion, b_retry.completion_id, &contexts[2],
              "reply-b"))
            describe_completion_failure (
              &sync, "B REQUEST completion", b_completion,
              b_retry.completion_id, &contexts[2]);
    }

    if (!is_aborted (&sync)) {
        std::lock_guard<std::mutex> lock (sync.mutex);
        sync.release_a = true;
        sync.changed.notify_all ();
    }
    if (!is_aborted (&sync)) {
        const bool a_finished = wait_for_state (
          &sync, [&] { return sync.a_submitted || sync.abort; });
        if (!a_finished || !sync.a_submitted)
            abort_case (&sync, "timed out waiting for A record");
    }
    if (!is_aborted (&sync)) {
        const bool received_a = wait_for_state (
          &sync, [&] { return sync.a_received || sync.abort; });
        if (!received_a || !sync.a_received)
            abort_case (&sync, "timed out waiting for intact A record");
    }
    if (!is_aborted (&sync) && request_) {
        a_completion = receive_completion (dealer);
        if (!completion_matches_request (
              a_completion, a_submit.completion_id, &contexts[3],
              "reply-a"))
            describe_completion_failure (
              &sync, "A REQUEST completion", a_completion,
              a_submit.completion_id, &contexts[3]);
    }

    if (is_aborted (&sync)) {
        std::lock_guard<std::mutex> lock (sync.mutex);
        sync.release_a = true;
        sync.changed.notify_all ();
    }
    application_a.join ();
    if (receiver.joinable ())
        receiver.join ();
    if (poller) {
        zlink_poller_remove (poller, dealer);
        zlink_poller_destroy (&poller);
    }

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);

    std::string failure;
    {
        std::lock_guard<std::mutex> lock (sync.failure_mutex);
        failure = sync.failure;
    }
    if (!failure.empty ()) {
        snprintf (failure_out_, failure_capacity_, "%s: %s", transport_,
                  failure.c_str ());
        return false;
    }
    failure_out_[0] = '\0';
    return true;
}
}

void test_writable_resubmit_from_other_thread_while_sequence_open ()
{
    const bool inproc =
      run_case ("inproc", true, 1, case_failures[0], sizeof (case_failures[0]));
    const bool tcp =
      run_case ("tcp", true, 2, case_failures[1], sizeof (case_failures[1]));
    if (!inproc || !tcp) {
        static char failure[1100];
        snprintf (failure, sizeof (failure), "%s%s%s",
                  inproc ? "" : case_failures[0],
                  !inproc && !tcp ? "; " : "",
                  tcp ? "" : case_failures[1]);
        TEST_FAIL_MESSAGE (failure);
    }
}

void test_writable_resubmit_from_other_thread_while_sequence_open_send ()
{
    const bool inproc = run_case ("inproc", false, 3, case_failures[2],
                                  sizeof (case_failures[2]));
    const bool tcp = run_case ("tcp", false, 4, case_failures[3],
                               sizeof (case_failures[3]));
    if (!inproc || !tcp) {
        static char failure[1100];
        snprintf (failure, sizeof (failure), "%s%s%s",
                  inproc ? "" : case_failures[2],
                  !inproc && !tcp ? "; " : "",
                  tcp ? "" : case_failures[3]);
        TEST_FAIL_MESSAGE (failure);
    }
}

int main ()
{
    setup_test_environment (60);
    UNITY_BEGIN ();
    if (selected (
          "test_writable_resubmit_from_other_thread_while_sequence_open"))
        RUN_TEST (
          test_writable_resubmit_from_other_thread_while_sequence_open);
    if (selected (
          "test_writable_resubmit_from_other_thread_while_sequence_open_send"))
        RUN_TEST (
          test_writable_resubmit_from_other_thread_while_sequence_open_send);
    return UNITY_END ();
}
