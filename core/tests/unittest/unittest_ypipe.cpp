/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include <ypipe.hpp>
#include <ypipe_conflate.hpp>

#include <unity.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

namespace
{
std::atomic<bool> count_allocations (false);
std::atomic<size_t> allocation_count (0);
std::atomic<long> live_allocations (0);
}

void *operator new (size_t size_, const std::nothrow_t &) noexcept
{
    if (count_allocations.load (std::memory_order_relaxed)) {
        allocation_count.fetch_add (1, std::memory_order_relaxed);
        live_allocations.fetch_add (1, std::memory_order_relaxed);
    }
    return malloc (size_);
}

void *operator new (size_t size_)
{
    void *const result = ::operator new (size_, std::nothrow);
    if (!result)
        throw std::bad_alloc ();
    return result;
}

void *operator new[] (size_t size_)
{
    return ::operator new (size_);
}

void operator delete (void *value_) noexcept
{
    if (value_ && count_allocations.load (std::memory_order_relaxed))
        live_allocations.fetch_sub (1, std::memory_order_relaxed);
    free (value_);
}

void operator delete[] (void *value_) noexcept
{
    ::operator delete (value_);
}

#if __cplusplus >= 201402L
void operator delete (void *value_, size_t) noexcept
{
    ::operator delete (value_);
}

void operator delete[] (void *value_, size_t) noexcept
{
    ::operator delete (value_);
}
#endif

void setUp ()
{
}
void tearDown ()
{
}

void test_create ()
{
    zlink::ypipe_t<int, 1> ypipe;
}

void test_check_read_empty ()
{
    zlink::ypipe_t<int, 1> ypipe;
    TEST_ASSERT_FALSE (ypipe.check_read ());
}

void test_read_empty ()
{
    zlink::ypipe_t<int, 1> ypipe;
    int read_value = -1;
    TEST_ASSERT_FALSE (ypipe.read (&read_value));
    TEST_ASSERT_EQUAL (-1, read_value);
}

void test_write_complete_and_check_read_and_read ()
{
    const int value = 42;
    zlink::ypipe_t<int, 1> ypipe;
    ypipe.write (value, false);
    TEST_ASSERT_FALSE (ypipe.check_read ());
    int read_value = -1;
    TEST_ASSERT_FALSE (ypipe.read (&read_value));
    TEST_ASSERT_EQUAL_INT (-1, read_value);
}

void test_write_complete_and_flush_and_check_read_and_read ()
{
    const int value = 42;
    zlink::ypipe_t<int, 1> ypipe;
    ypipe.write (value, false);
    ypipe.flush ();
    TEST_ASSERT_TRUE (ypipe.check_read ());
    int read_value = -1;
    TEST_ASSERT_TRUE (ypipe.read (&read_value));
    TEST_ASSERT_EQUAL_INT (value, read_value);
}

void test_read_reports_prefetched_batch_tail ()
{
    zlink::ypipe_t<int, 1> ypipe;
    ypipe.write (1, false);
    ypipe.write (2, false);
    ypipe.write (3, false);
    ypipe.flush ();

    int read_value = -1;
    bool batch_tail = true;
    TEST_ASSERT_TRUE (ypipe.read (&read_value, &batch_tail));
    TEST_ASSERT_EQUAL_INT (1, read_value);
    TEST_ASSERT_FALSE (batch_tail);
    TEST_ASSERT_TRUE (ypipe.read (&read_value, &batch_tail));
    TEST_ASSERT_EQUAL_INT (2, read_value);
    TEST_ASSERT_FALSE (batch_tail);
    TEST_ASSERT_TRUE (ypipe.read (&read_value, &batch_tail));
    TEST_ASSERT_EQUAL_INT (3, read_value);
    TEST_ASSERT_TRUE (batch_tail);
}

struct int_read_context_t
{
    int expected;
    bool accept;
    int calls;
};

bool read_int_if (const int &value_, void *userdata_)
{
    int_read_context_t *const context =
      static_cast<int_read_context_t *> (userdata_);
    TEST_ASSERT_EQUAL_INT (context->expected, value_);
    ++context->calls;
    return context->accept;
}

void probe_int (const int &value_, void *userdata_)
{
    int_read_context_t *const context =
      static_cast<int_read_context_t *> (userdata_);
    TEST_ASSERT_EQUAL_INT (context->expected, value_);
    ++context->calls;
}

void test_published_probe_does_not_put_empty_reader_to_sleep ()
{
    zlink::ypipe_t<int, 1> ypipe;
    int_read_context_t context = {42, false, 0};

    TEST_ASSERT_FALSE (ypipe.probe_if_published (&probe_int, &context));
    TEST_ASSERT_EQUAL_INT (0, context.calls);

    ypipe.write (context.expected, false);
    TEST_ASSERT_TRUE_MESSAGE (
      ypipe.flush (),
      "empty published-head probe incorrectly put the reader to sleep");
    TEST_ASSERT_TRUE (ypipe.probe_if_published (&probe_int, &context));
    TEST_ASSERT_EQUAL_INT (1, context.calls);

    int read_value = -1;
    TEST_ASSERT_TRUE (ypipe.read (&read_value));
    TEST_ASSERT_EQUAL_INT (context.expected, read_value);
}

void test_read_if_distinguishes_empty_rejected_and_consumed ()
{
    zlink::ypipe_t<int, 1> ypipe;
    int read_value = -1;
    int_read_context_t context = {42, false, 0};
    bool batch_tail = true;

    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_empty,
      ypipe.read_if (&read_value, &read_int_if, &context, &batch_tail));
    TEST_ASSERT_EQUAL_INT (0, context.calls);
    TEST_ASSERT_EQUAL_INT (-1, read_value);
    TEST_ASSERT_FALSE (batch_tail);

    ypipe.write (context.expected, false);
    ypipe.flush ();
    batch_tail = true;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_rejected,
      ypipe.read_if (&read_value, &read_int_if, &context, &batch_tail));
    TEST_ASSERT_EQUAL_INT (1, context.calls);
    TEST_ASSERT_EQUAL_INT (-1, read_value);
    TEST_ASSERT_FALSE (batch_tail);
    TEST_ASSERT_TRUE (ypipe.check_read ());

    context.accept = true;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_consumed,
      ypipe.read_if (&read_value, &read_int_if, &context));
    TEST_ASSERT_EQUAL_INT (2, context.calls);
    TEST_ASSERT_EQUAL_INT (context.expected, read_value);
    TEST_ASSERT_FALSE (ypipe.check_read ());
}

void test_read_if_reports_prefetched_batch_tail ()
{
    zlink::ypipe_t<int, 1> ypipe;
    ypipe.write (41, false);
    ypipe.write (42, false);
    ypipe.flush ();

    int read_value = -1;
    int_read_context_t context = {41, true, 0};
    bool batch_tail = true;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_consumed,
      ypipe.read_if (&read_value, &read_int_if, &context, &batch_tail));
    TEST_ASSERT_FALSE (batch_tail);

    context.expected = 42;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_consumed,
      ypipe.read_if (&read_value, &read_int_if, &context, &batch_tail));
    TEST_ASSERT_TRUE (batch_tail);
}

struct msg_read_context_t
{
    unsigned char expected;
    bool accept;
    int calls;
};

bool read_msg_if (const zlink::msg_t &msg_, void *userdata_)
{
    msg_read_context_t *const context =
      static_cast<msg_read_context_t *> (userdata_);
    TEST_ASSERT_EQUAL_UINT (1, msg_.size ());
    TEST_ASSERT_EQUAL_HEX8 (
      context->expected,
      *static_cast<const unsigned char *> (
        const_cast<zlink::msg_t &> (msg_).data ()));
    ++context->calls;
    return context->accept;
}

void test_conflate_read_if_retains_or_consumes_under_one_decision ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> ypipe;
    zlink::msg_t first;
    zlink::msg_t replacement;
    zlink::msg_t read_value;
    TEST_ASSERT_EQUAL_INT (0, first.init_size (1));
    TEST_ASSERT_EQUAL_INT (0, replacement.init_size (1));
    TEST_ASSERT_EQUAL_INT (0, read_value.init ());
    *static_cast<unsigned char *> (first.data ()) = 0x11;
    *static_cast<unsigned char *> (replacement.data ()) = 0x22;

    ypipe.write (first, false);
    ypipe.write (replacement, false);
    ypipe.flush ();
    msg_read_context_t context = {0x22, false, 0};
    bool batch_tail = true;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_rejected,
      ypipe.read_if (&read_value, &read_msg_if, &context, &batch_tail));
    TEST_ASSERT_EQUAL_INT (1, context.calls);
    TEST_ASSERT_EQUAL_UINT (0, read_value.size ());
    TEST_ASSERT_FALSE (batch_tail);

    context.accept = true;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_consumed,
      ypipe.read_if (&read_value, &read_msg_if, &context, &batch_tail));
    TEST_ASSERT_EQUAL_INT (2, context.calls);
    TEST_ASSERT_EQUAL_HEX8 (
      context.expected,
      *static_cast<unsigned char *> (read_value.data ()));
    TEST_ASSERT_TRUE (batch_tail);
    TEST_ASSERT_EQUAL_INT (0, read_value.close ());

    TEST_ASSERT_EQUAL_INT (0, read_value.init ());
    batch_tail = true;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_empty,
      ypipe.read_if (&read_value, &read_msg_if, &context, &batch_tail));
    TEST_ASSERT_EQUAL_INT (2, context.calls);
    TEST_ASSERT_FALSE (batch_tail);
    TEST_ASSERT_EQUAL_INT (0, read_value.close ());

    TEST_ASSERT_EQUAL_INT (0, first.close ());
    TEST_ASSERT_EQUAL_INT (0, replacement.close ());
}

namespace
{
void free_conflate_frame (void *data_, void *hint_)
{
    ++*static_cast<int *> (hint_);
    free (data_);
}

uint64_t conflate_frame_bytes (const zlink::msg_t &msg_)
{
    return msg_.size ();
}

bool conflate_complete_message (const zlink::msg_t &msg_)
{
    return (msg_.flags () & zlink::msg_t::more) == 0;
}

void write_conflate_frame (
  zlink::ypipe_conflate_t<zlink::msg_t> &pipe_, unsigned char value_, bool more_,
  int *freed_, zlink::ypipe_replacement_accounting_t *replaced_)
{
    void *data = malloc (128);
    TEST_ASSERT_NOT_NULL (data);
    memset (data, value_, 128);
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (
      0, msg.init_data (data, 128, free_conflate_frame, freed_));
    if (more_)
        msg.set_flags (zlink::msg_t::more);
    pipe_.write_with_replacement_accounting (
      msg, more_, conflate_frame_bytes, conflate_complete_message, replaced_);
    pipe_.flush ();
    // A successful pipe write takes the reference, just like pipe_t::write.
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void read_conflate_frame (zlink::ypipe_conflate_t<zlink::msg_t> &pipe_,
                         unsigned char expected_, bool more_)
{
    zlink::msg_t msg;
    bool batch_tail = more_;
    TEST_ASSERT_TRUE (pipe_.read (&msg, &batch_tail));
    TEST_ASSERT_EQUAL_UINT (128, msg.size ());
    TEST_ASSERT_EQUAL_HEX8 (expected_,
                           *static_cast<unsigned char *> (msg.data ()));
    TEST_ASSERT_EQUAL_INT (more_, (msg.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_EQUAL_INT (!more_, batch_tail);
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}
}

void test_conflate_complete_records_topics_rollback_and_ownership ()
{
    int freed = 0;
    {
        zlink::ypipe_conflate_t<zlink::msg_t> pipe;
        zlink::ypipe_replacement_accounting_t replaced;
        write_conflate_frame (pipe, 'A', true, &freed, &replaced);
        pipe.flush ();
        TEST_ASSERT_FALSE (pipe.check_read ());
        write_conflate_frame (pipe, '1', false, &freed, &replaced);
        write_conflate_frame (pipe, 'B', true, &freed, &replaced);
        write_conflate_frame (pipe, '2', false, &freed, &replaced);
        TEST_ASSERT_EQUAL_UINT64 (0, replaced.bytes);

        // Once A's topic is read, its payload cannot be replaced by A3.
        read_conflate_frame (pipe, 'A', true);
        write_conflate_frame (pipe, 'A', true, &freed, &replaced);
        write_conflate_frame (pipe, '3', false, &freed, &replaced);
        TEST_ASSERT_EQUAL_UINT64 (0, replaced.bytes);
        write_conflate_frame (pipe, 'B', true, &freed, &replaced);
        TEST_ASSERT_EQUAL_UINT64 (0, replaced.bytes);
        write_conflate_frame (pipe, '4', false, &freed, &replaced);
        TEST_ASSERT_EQUAL_UINT64 (256, replaced.bytes);
        TEST_ASSERT_EQUAL_UINT64 (1, replaced.complete_messages);
        TEST_ASSERT_EQUAL_INT (3, freed); // read A topic + replaced B2

        // A failed multipart send rolls back its prefix, not published data.
        write_conflate_frame (pipe, 'C', true, &freed, &replaced);
        zlink::msg_t rollback;
        TEST_ASSERT_TRUE (pipe.unwrite (&rollback));
        TEST_ASSERT_EQUAL_HEX8 (
          'C', *static_cast<unsigned char *> (rollback.data ()));
        TEST_ASSERT_EQUAL_INT (0, rollback.close ());
        TEST_ASSERT_FALSE (pipe.unwrite (&rollback));
        // B4 keeps B2's position; A3 followed the started A1.
        read_conflate_frame (pipe, '1', false);
        read_conflate_frame (pipe, 'B', true);
        read_conflate_frame (pipe, '4', false);
        read_conflate_frame (pipe, 'A', true);
        read_conflate_frame (pipe, '3', false);
        TEST_ASSERT_FALSE (pipe.check_read ());
        TEST_ASSERT_EQUAL_INT (9, freed);

        // Shutdown charges only remaining committed frames; provisional
        // frames are released by pipe rollback. Both references are freed.
        write_conflate_frame (pipe, 'A', true, &freed, &replaced);
        write_conflate_frame (pipe, '5', false, &freed, &replaced);
        read_conflate_frame (pipe, 'A', true);
        write_conflate_frame (pipe, 'B', true, &freed, &replaced);
        pipe.discard_accounting (conflate_frame_bytes,
                                 conflate_complete_message, &replaced);
        TEST_ASSERT_EQUAL_UINT64 (128, replaced.bytes);
        TEST_ASSERT_EQUAL_UINT64 (1, replaced.complete_messages);
    }
    TEST_ASSERT_EQUAL_INT (12, freed);
}

void test_conflate_concurrent_multipart_and_wake ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    std::atomic<bool> writer_done (false);
    const unsigned records = 10000;
    std::thread writer ([&] () {
        for (unsigned i = 0; i != records; ++i) {
            zlink::msg_t topic;
            zlink::msg_t payload;
            zlink_assert (topic.init_size (1) == 0);
            zlink_assert (payload.init_size (sizeof (i)) == 0);
            *static_cast<char *> (topic.data ()) = 'A';
            memcpy (payload.data (), &i, sizeof (i));
            topic.set_flags (zlink::msg_t::more);
            pipe.write (topic, true);
            pipe.write (payload, false);
            pipe.flush ();
            zlink_assert (topic.init () == 0);
            zlink_assert (payload.init () == 0);
        }
        writer_done.store (true, std::memory_order_release);
    });

    unsigned last = 0;
    unsigned received = 0;
    bool valid = true;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (!writer_done.load (std::memory_order_acquire) || pipe.check_read ()) {
        if (std::chrono::steady_clock::now () >= deadline) {
            valid = false;
            break;
        }
        zlink::msg_t topic;
        if (!pipe.read (&topic)) {
            std::this_thread::yield ();
            continue;
        }
        valid &= topic.size () == 1 && *static_cast<char *> (topic.data ()) == 'A'
                 && (topic.flags () & zlink::msg_t::more) != 0;
        valid &= topic.close () == 0;
        zlink::msg_t payload;
        if (!pipe.read (&payload)) {
            valid = false;
            continue;
        }
        unsigned sequence = 0;
        valid &=
          payload.size () == sizeof (sequence) && (payload.flags () & zlink::msg_t::more) == 0;
        if (payload.size () == sizeof (sequence))
            memcpy (&sequence, payload.data (), sizeof (sequence));
        valid &= received == 0 || sequence > last;
        valid &= payload.close () == 0;
        last = sequence;
        ++received;
    }
    writer.join ();
    TEST_ASSERT_TRUE (valid);
    TEST_ASSERT_TRUE (received > 0);
    TEST_ASSERT_EQUAL_UINT (records - 1, last);
}

void test_conflate_empty_reader_wake_exchange ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    std::atomic<unsigned> ready (0);
    std::atomic<unsigned> notified (0);
    std::atomic<unsigned> consumed (0);
    std::atomic<bool> stop (false);
    std::atomic<bool> missed_wake (false);
    std::thread writer ([&] () {
        for (unsigned i = 0; i != 1000; ++i) {
            while (ready.load (std::memory_order_acquire) != i + 1
                   && !stop.load (std::memory_order_acquire))
                std::this_thread::yield ();
            if (stop.load (std::memory_order_acquire))
                return;
            zlink::msg_t msg;
            zlink_assert (msg.init_size (1) == 0);
            *static_cast<unsigned char *> (msg.data ()) = static_cast<unsigned char> (i);
            pipe.write (msg, false);
            if (pipe.flush ())
                missed_wake.store (true, std::memory_order_release);
            zlink_assert (msg.init () == 0);
            notified.store (i + 1, std::memory_order_release);
            while (consumed.load (std::memory_order_acquire) != i + 1
                   && !stop.load (std::memory_order_acquire))
                std::this_thread::yield ();
            if (stop.load (std::memory_order_acquire))
                return;
        }
    });
    bool valid = true;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    for (unsigned i = 0; i != 1000; ++i) {
        valid &= !pipe.check_read ();
        ready.store (i + 1, std::memory_order_release);
        while (notified.load (std::memory_order_acquire) != i + 1
               && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        if (notified.load (std::memory_order_acquire) != i + 1) {
            valid = false;
            break;
        }
        zlink::msg_t msg;
        if (!pipe.read (&msg)) {
            valid = false;
            break;
        }
        valid &= msg.size () == 1
                 && *static_cast<unsigned char *> (msg.data ()) == static_cast<unsigned char> (i);
        valid &= msg.close () == 0;
        consumed.store (i + 1, std::memory_order_release);
    }
    stop.store (true, std::memory_order_release);
    writer.join ();
    TEST_ASSERT_TRUE (valid);
    TEST_ASSERT_FALSE (missed_wake.load (std::memory_order_acquire));
}

void test_conflate_publication_races_empty_observation ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    std::atomic<unsigned> start (0);
    std::atomic<unsigned> published (0);
    std::atomic<unsigned> consumed (0);
    std::atomic<bool> stop (false);
    std::atomic<bool> wake_sent (false);
    std::thread writer ([&] () {
        for (unsigned i = 0; i != 10000; ++i) {
            while (start.load (std::memory_order_acquire) != i + 1
                   && !stop.load (std::memory_order_acquire))
                std::this_thread::yield ();
            if (stop.load (std::memory_order_acquire))
                return;
            zlink::msg_t msg;
            zlink_assert (msg.init_size (1) == 0);
            *static_cast<unsigned char *> (msg.data ()) = static_cast<unsigned char> (i);
            pipe.write (msg, false);
            wake_sent.store (!pipe.flush (), std::memory_order_release);
            zlink_assert (msg.init () == 0);
            published.store (i + 1, std::memory_order_release);
            while (consumed.load (std::memory_order_acquire) != i + 1
                   && !stop.load (std::memory_order_acquire))
                std::this_thread::yield ();
            if (stop.load (std::memory_order_acquire))
                return;
        }
    });
    bool valid = true;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    for (unsigned i = 0; i != 10000; ++i) {
        start.store (i + 1, std::memory_order_release);
        const bool readable = pipe.check_read ();
        while (published.load (std::memory_order_acquire) != i + 1
               && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        if (published.load (std::memory_order_acquire) != i + 1) {
            valid = false;
            break;
        }
        valid &= readable || wake_sent.load (std::memory_order_acquire);
        zlink::msg_t msg;
        if (!pipe.read (&msg)) {
            valid = false;
            break;
        }
        valid &= msg.size () == 1
                 && *static_cast<unsigned char *> (msg.data ())
                      == static_cast<unsigned char> (i);
        valid &= msg.close () == 0;
        consumed.store (i + 1, std::memory_order_release);
    }
    stop.store (true, std::memory_order_release);
    writer.join ();
    TEST_ASSERT_TRUE (valid);
}

namespace
{
void write_topic_record (zlink::ypipe_conflate_t<zlink::msg_t> &pipe_,
                         const void *topic_, size_t topic_size_,
                         unsigned char payload_,
                         zlink::ypipe_replacement_accounting_t *replaced_)
{
    zlink::msg_t topic;
    zlink::msg_t payload;
    zlink_assert (topic.init_size (topic_size_) == 0);
    zlink_assert (payload.init_size (1) == 0);
    memcpy (topic.data (), topic_, topic_size_);
    *static_cast<unsigned char *> (payload.data ()) = payload_;
    topic.set_flags (zlink::msg_t::more);
    pipe_.write_with_replacement_accounting (
      topic, true, conflate_frame_bytes, conflate_complete_message, replaced_);
    pipe_.write_with_replacement_accounting (
      payload, false, conflate_frame_bytes, conflate_complete_message,
      replaced_);
    pipe_.flush ();
    zlink_assert (topic.init () == 0);
    zlink_assert (payload.init () == 0);
}

bool read_topic_record (zlink::ypipe_conflate_t<zlink::msg_t> &pipe_,
                        char topic_, unsigned char payload_)
{
    zlink::msg_t topic;
    zlink::msg_t payload;
    if (!pipe_.read (&topic))
        return false;
    const bool topic_ok =
      topic.size () == 1 && *static_cast<char *> (topic.data ()) == topic_;
    zlink_assert (topic.close () == 0);
    if (!pipe_.read (&payload))
        return false;
    const bool payload_ok =
      payload.size () == 1
      && *static_cast<unsigned char *> (payload.data ()) == payload_;
    zlink_assert (payload.close () == 0);
    return topic_ok && payload_ok;
}

struct reject_during_replacement_t
{
    std::atomic<bool> entered;
    std::atomic<bool> proceed;
};

bool reject_during_replacement (const zlink::msg_t &, void *userdata_)
{
    reject_during_replacement_t *const state =
      static_cast<reject_during_replacement_t *> (userdata_);
    state->entered.store (true, std::memory_order_release);
    while (!state->proceed.load (std::memory_order_acquire))
        std::this_thread::yield ();
    return false;
}

void probe_during_replacement (const zlink::msg_t &msg_, void *userdata_)
{
    reject_during_replacement (msg_, userdata_);
}

//  Runs `claim_` on a reader thread and publishes A2 while the reader holds
//  A1 inside the callback, that is between its claim and its return of A1.
template <typename Claim>
void publish_while_claimed (zlink::ypipe_conflate_t<zlink::msg_t> &pipe_,
                            reject_during_replacement_t *state_, Claim claim_,
                            unsigned char payload_,
                            zlink::ypipe_replacement_accounting_t *replaced_)
{
    state_->entered.store (false, std::memory_order_relaxed);
    state_->proceed.store (false, std::memory_order_relaxed);
    std::thread reader (claim_);
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (!state_->entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    const bool entered = state_->entered.load (std::memory_order_acquire);
    if (entered)
        write_topic_record (pipe_, "A", 1, payload_, replaced_);
    state_->proceed.store (true, std::memory_order_release);
    reader.join ();
    TEST_ASSERT_TRUE_MESSAGE (entered, "the reader did not claim the record");
}

//  The reader has claimed A1 for a conditional read or a probe that does
//  not start it; the writer publishes A2 before the reader returns A1.
void check_claim_racing_replacement (bool probe_)
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    zlink::ypipe_replacement_accounting_t replaced;
    write_topic_record (pipe, "A", 1, '1', &replaced);
    write_topic_record (pipe, "B", 1, '1', &replaced);
    TEST_ASSERT_EQUAL_UINT64 (0, replaced.bytes);

    reject_during_replacement_t state;
    zlink::ypipe_read_result_t result = zlink::ypipe_read_empty;
    bool probed = false;
    publish_while_claimed (
      pipe, &state,
      [&] () {
          if (probe_) {
              probed =
                pipe.probe_if_published (probe_during_replacement, &state);
          } else {
              zlink::msg_t ignored;
              result =
                pipe.read_if (&ignored, reject_during_replacement, &state);
              ignored.close ();
          }
      },
      '2', &replaced);
    // A1 was claimed, so the writer could not replace it and queued the cell.
    TEST_ASSERT_EQUAL_UINT64 (0, replaced.bytes);
    if (probe_) {
        TEST_ASSERT_TRUE (probed);
    } else {
        TEST_ASSERT_EQUAL_INT (zlink::ypipe_read_rejected, result);
    }

    // A2 takes A1's position, and A has exactly one unread record.
    TEST_ASSERT_TRUE (read_topic_record (pipe, 'A', '2'));
    TEST_ASSERT_TRUE (read_topic_record (pipe, 'B', '1'));
    TEST_ASSERT_FALSE (pipe.check_read ());

    // The superseded A1 is charged once, as a replacement, by the next write.
    write_topic_record (pipe, "C", 1, '1', &replaced);
    TEST_ASSERT_EQUAL_UINT64 (2, replaced.bytes);
    TEST_ASSERT_EQUAL_UINT64 (1, replaced.complete_messages);
    write_topic_record (pipe, "D", 1, '1', &replaced);
    TEST_ASSERT_EQUAL_UINT64 (0, replaced.bytes);

    // The writer's stale queue entry for A is skipped, and a later A is
    // received after the records published before it.
    write_topic_record (pipe, "A", 1, '3', &replaced);
    TEST_ASSERT_TRUE (read_topic_record (pipe, 'C', '1'));
    TEST_ASSERT_TRUE (read_topic_record (pipe, 'D', '1'));
    TEST_ASSERT_TRUE (read_topic_record (pipe, 'A', '3'));
    TEST_ASSERT_FALSE (pipe.check_read ());
    pipe.discard_accounting (conflate_frame_bytes, conflate_complete_message,
                             &replaced);
    TEST_ASSERT_EQUAL_UINT64 (0, replaced.bytes);
}
}

void test_conflate_reject_racing_replacement_keeps_position_and_charge ()
{
    check_claim_racing_replacement (false);
}

void test_conflate_probe_racing_replacement_keeps_position_and_charge ()
{
    check_claim_racing_replacement (true);
}

//  Without a later write, discard returns the superseded record's charge.
void test_conflate_racing_replacement_discard_returns_charge ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    zlink::ypipe_replacement_accounting_t replaced;
    write_topic_record (pipe, "A", 1, '1', &replaced);
    reject_during_replacement_t state;
    publish_while_claimed (
      pipe, &state,
      [&] () {
          zlink::msg_t ignored;
          pipe.read_if (&ignored, reject_during_replacement, &state);
          ignored.close ();
      },
      '2', &replaced);
    // Superseded A1 and unread A2 remain charged, 2 bytes each.
    pipe.discard_accounting (conflate_frame_bytes, conflate_complete_message,
                             &replaced);
    TEST_ASSERT_EQUAL_UINT64 (4, replaced.bytes);
    TEST_ASSERT_EQUAL_UINT64 (2, replaced.complete_messages);
}

//  Two claims of the same front each lose to a publication, so the front
//  cell has two stale queue entries.
void test_conflate_two_racing_replacements_skip_two_stale_entries ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    zlink::ypipe_replacement_accounting_t replaced;
    write_topic_record (pipe, "A", 1, '1', &replaced);
    write_topic_record (pipe, "B", 1, '1', &replaced);
    reject_during_replacement_t state;
    const auto reject_front = [&] () {
        zlink::msg_t ignored;
        pipe.read_if (&ignored, reject_during_replacement, &state);
        ignored.close ();
    };
    publish_while_claimed (pipe, &state, reject_front, '2', &replaced);
    TEST_ASSERT_EQUAL_UINT64 (0, replaced.bytes);
    publish_while_claimed (pipe, &state, reject_front, '3', &replaced);
    // A2 was superseded while claimed; A1 is returned by the same write.
    TEST_ASSERT_EQUAL_UINT64 (2, replaced.bytes);
    TEST_ASSERT_EQUAL_UINT64 (1, replaced.complete_messages);

    TEST_ASSERT_TRUE (read_topic_record (pipe, 'A', '3'));
    TEST_ASSERT_TRUE (read_topic_record (pipe, 'B', '1'));
    TEST_ASSERT_FALSE (pipe.check_read ());
    write_topic_record (pipe, "C", 1, '1', &replaced);
    TEST_ASSERT_EQUAL_UINT64 (2, replaced.bytes);
    TEST_ASSERT_EQUAL_UINT64 (1, replaced.complete_messages);
    write_topic_record (pipe, "A", 1, '4', &replaced);
    TEST_ASSERT_TRUE (read_topic_record (pipe, 'C', '1'));
    TEST_ASSERT_TRUE (read_topic_record (pipe, 'A', '4'));
    TEST_ASSERT_FALSE (pipe.check_read ());
}

namespace
{
const unsigned stress_topics = 4;

unsigned stress_topic_of (unsigned i_)
{
    return (i_ / 3) % stress_topics;
}

bool reject_every_third (const zlink::msg_t &, void *userdata_)
{
    unsigned *const calls = static_cast<unsigned *> (userdata_);
    return ++*calls % 3 != 0;
}

void ignore_probe (const zlink::msg_t &, void *)
{
}

//  One writer and one reader that mixes probes, rejections and reads.
bool run_conflate_stress_round ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    const unsigned records = 20000;
    std::atomic<bool> done (false);
    uint64_t written_bytes = 0;
    uint64_t replaced_bytes = 0;
    std::thread writer ([&] () {
        for (unsigned i = 0; i != records; ++i) {
            const unsigned char topic =
              static_cast<unsigned char> ('A' + stress_topic_of (i));
            zlink::msg_t topic_frame;
            zlink::msg_t payload;
            zlink_assert (topic_frame.init_size (1) == 0);
            zlink_assert (payload.init_size (sizeof (i)) == 0);
            *static_cast<unsigned char *> (topic_frame.data ()) = topic;
            memcpy (payload.data (), &i, sizeof (i));
            topic_frame.set_flags (zlink::msg_t::more);
            zlink::ypipe_replacement_accounting_t replaced;
            pipe.write_with_replacement_accounting (
              topic_frame, true, conflate_frame_bytes,
              conflate_complete_message, &replaced);
            replaced_bytes += replaced.bytes;
            pipe.write_with_replacement_accounting (
              payload, false, conflate_frame_bytes, conflate_complete_message,
              &replaced);
            replaced_bytes += replaced.bytes;
            written_bytes += 1 + sizeof (i);
            pipe.flush ();
            zlink_assert (topic_frame.init () == 0);
            zlink_assert (payload.init () == 0);
            if (i % 17 == 0)
                std::this_thread::yield ();
        }
        done.store (true, std::memory_order_release);
    });
    unsigned last[stress_topics] = {0};
    bool seen[stress_topics] = {false};
    unsigned after_writer[stress_topics] = {0};
    uint64_t read_bytes = 0;
    unsigned operations = 0;
    unsigned admissions = 0;
    bool valid = true;
    while (true) {
        const bool writer_done = done.load (std::memory_order_acquire);
        if (++operations % 5 == 0) {
            pipe.probe_if_published (ignore_probe, NULL);
            continue;
        }
        zlink::msg_t topic;
        topic.init ();
        const zlink::ypipe_read_result_t result =
          pipe.read_if (&topic, reject_every_third, &admissions);
        if (result == zlink::ypipe_read_empty) {
            if (writer_done && !pipe.check_read ())
                break;
            std::this_thread::yield ();
            continue;
        }
        if (result == zlink::ypipe_read_rejected)
            continue;
        valid &= topic.size () == 1
                 && (topic.flags () & zlink::msg_t::more) != 0;
        const unsigned t = *static_cast<unsigned char *> (topic.data ()) - 'A';
        read_bytes += topic.size ();
        topic.close ();
        zlink::msg_t payload;
        payload.init ();
        if (!pipe.read (&payload)) {
            valid = false;
            break;
        }
        unsigned sequence = 0;
        valid &= payload.size () == sizeof (sequence);
        memcpy (&sequence, payload.data (), sizeof (sequence));
        read_bytes += payload.size ();
        payload.close ();
        if (t >= stress_topics) {
            valid = false;
            continue;
        }
        // Per-topic order is monotonic, and after the writer ends each
        // topic has at most one unread record.
        valid &= stress_topic_of (sequence) == t;
        valid &= !seen[t] || sequence > last[t];
        seen[t] = true;
        last[t] = sequence;
        if (writer_done)
            valid &= ++after_writer[t] <= 1;
    }
    writer.join ();
    // The last record of every topic is delivered.
    for (unsigned t = 0; t != stress_topics; ++t) {
        unsigned expected = 0;
        for (unsigned i = 0; i != records; ++i)
            if (stress_topic_of (i) == t)
                expected = i;
        valid &= seen[t] && last[t] == expected;
    }
    zlink::ypipe_replacement_accounting_t discarded;
    pipe.discard_accounting (conflate_frame_bytes, conflate_complete_message,
                             &discarded);
    return valid
           && written_bytes == read_bytes + replaced_bytes + discarded.bytes;
}
}

void test_conflate_mixed_reader_stress_conserves_charge ()
{
    int failed_rounds = 0;
    for (int round = 0; round != 30; ++round)
        if (!run_conflate_stress_round ())
            ++failed_rounds;
    TEST_ASSERT_EQUAL_INT (0, failed_rounds);
}

void test_conflate_distinct_topics_consumed_one_at_a_time_stay_bounded ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    zlink::ypipe_replacement_accounting_t replaced;
    live_allocations.store (0, std::memory_order_relaxed);
    count_allocations.store (true, std::memory_order_relaxed);
    long peak = 0;
    for (unsigned i = 0; i != 10000; ++i) {
        unsigned char key[sizeof (i)];
        memcpy (key, &i, sizeof (i));
        write_topic_record (pipe, key, sizeof (key), '1', &replaced);
        zlink::msg_t frame;
        zlink_assert (pipe.read (&frame));
        zlink_assert (frame.close () == 0);
        zlink_assert (pipe.read (&frame));
        zlink_assert (frame.close () == 0);
        const long live = live_allocations.load (std::memory_order_relaxed);
        if (live > peak)
            peak = live;
    }
    count_allocations.store (false, std::memory_order_relaxed);
    TEST_ASSERT_FALSE (pipe.check_read ());
    // At most the pending frame vector, the index table, and the last
    // returned cell and record stay allocated.
    TEST_ASSERT_LESS_OR_EQUAL_INT (4, peak);
}

namespace
{
void write_single_record (zlink::ypipe_conflate_t<zlink::msg_t> &pipe_,
                          unsigned char value_)
{
    zlink::msg_t msg;
    zlink_assert (msg.init_size (1) == 0);
    *static_cast<unsigned char *> (msg.data ()) = value_;
    zlink::ypipe_replacement_accounting_t replaced;
    pipe_.write_with_replacement_accounting (
      msg, false, conflate_frame_bytes, conflate_complete_message, &replaced);
    pipe_.flush ();
    zlink_assert (msg.init () == 0);
}

//  One cycle publishes a single-part record and multipart records of three
//  topics, then consumes them.
bool publish_consume_cycle (zlink::ypipe_conflate_t<zlink::msg_t> &pipe_,
                            unsigned i_)
{
    zlink::ypipe_replacement_accounting_t replaced;
    const unsigned char value = static_cast<unsigned char> (i_);
    write_single_record (pipe_, value);
    write_topic_record (pipe_, "A", 1, value, &replaced);
    write_topic_record (pipe_, "B", 1, value, &replaced);
    write_topic_record (pipe_, "A", 1, value, &replaced);
    write_topic_record (pipe_, "C", 1, value, &replaced);
    zlink::msg_t single;
    if (!pipe_.read (&single))
        return false;
    const bool single_ok =
      *static_cast<unsigned char *> (single.data ()) == value;
    zlink_assert (single.close () == 0);
    return single_ok && read_topic_record (pipe_, 'A', value)
           && read_topic_record (pipe_, 'B', value)
           && read_topic_record (pipe_, 'C', value) && !pipe_.check_read ();
}
}

//  After warm-up, records come from the spare list: neither a replacement
//  nor a publish/consume cycle allocates.
void test_conflate_steady_state_does_not_allocate ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    for (unsigned i = 0; i != 100; ++i)
        TEST_ASSERT_TRUE (publish_consume_cycle (pipe, i));
    allocation_count.store (0, std::memory_order_relaxed);
    count_allocations.store (true, std::memory_order_relaxed);
    bool valid = true;
    for (unsigned i = 0; i != 100000; ++i)
        valid &= publish_consume_cycle (pipe, i);
    count_allocations.store (false, std::memory_order_relaxed);
    TEST_ASSERT_TRUE (valid);
    TEST_ASSERT_EQUAL_UINT (0, allocation_count.load (std::memory_order_relaxed));

    // A writer without a reader replaces records in place of allocating.
    zlink::ypipe_replacement_accounting_t replaced;
    write_topic_record (pipe, "A", 1, 0, &replaced);
    allocation_count.store (0, std::memory_order_relaxed);
    count_allocations.store (true, std::memory_order_relaxed);
    for (unsigned i = 0; i != 100000; ++i) {
        write_single_record (pipe, static_cast<unsigned char> (i));
        write_topic_record (pipe, "A", 1, static_cast<unsigned char> (i),
                            &replaced);
    }
    count_allocations.store (false, std::memory_order_relaxed);
    TEST_ASSERT_EQUAL_UINT (0, allocation_count.load (std::memory_order_relaxed));
}

namespace
{
void write_numbered_topic (zlink::ypipe_conflate_t<zlink::msg_t> &pipe_,
                           unsigned topic_)
{
    zlink::ypipe_replacement_accounting_t replaced;
    unsigned char key[sizeof (topic_)];
    memcpy (key, &topic_, sizeof (topic_));
    write_topic_record (pipe_, key, sizeof (key), '1', &replaced);
}

void consume_record (zlink::ypipe_conflate_t<zlink::msg_t> &pipe_)
{
    zlink::msg_t frame;
    zlink_assert (pipe_.read (&frame));
    zlink_assert (frame.close () == 0);
    zlink_assert (pipe_.read (&frame));
    zlink_assert (frame.close () == 0);
}
}

//  A burst far above the spare cap, consumed later, leaves at most the
//  capped spares allocated.
void test_conflate_burst_of_distinct_topics_releases_beyond_spare_cap ()
{
    const long spare_cap = 64;
    live_allocations.store (0, std::memory_order_relaxed);
    count_allocations.store (true, std::memory_order_relaxed);
    long remaining = 0;
    {
        zlink::ypipe_conflate_t<zlink::msg_t> pipe;
        const unsigned burst = 10000;
        for (unsigned i = 0; i != burst; ++i)
            write_numbered_topic (pipe, i);
        for (unsigned i = 0; i != burst; ++i)
            consume_record (pipe);
        // The next write collects every returned record and cell.
        write_numbered_topic (pipe, burst);
        consume_record (pipe);
        TEST_ASSERT_FALSE (pipe.check_read ());
        remaining = live_allocations.load (std::memory_order_relaxed);
    }
    count_allocations.store (false, std::memory_order_relaxed);
    // Capped spare records and cells, the returned last record and cell,
    // the index table and the pending frame vector.
    TEST_ASSERT_LESS_OR_EQUAL_INT (2 * spare_cap + 4, remaining);
}

//  Repeated bursts that fit in the spare cap reuse cells and records, and
//  the index keeps the capacity that holds them.
void test_conflate_burst_cycles_within_spare_cap_do_not_allocate ()
{
    zlink::ypipe_conflate_t<zlink::msg_t> pipe;
    const unsigned burst = 48;
    for (unsigned cycle = 0; cycle != 10; ++cycle) {
        for (unsigned i = 0; i != burst; ++i)
            write_numbered_topic (pipe, cycle * burst + i);
        for (unsigned i = 0; i != burst; ++i)
            consume_record (pipe);
    }
    allocation_count.store (0, std::memory_order_relaxed);
    count_allocations.store (true, std::memory_order_relaxed);
    for (unsigned cycle = 10; cycle != 1010; ++cycle) {
        for (unsigned i = 0; i != burst; ++i)
            write_numbered_topic (pipe, cycle * burst + i);
        for (unsigned i = 0; i != burst; ++i)
            consume_record (pipe);
    }
    count_allocations.store (false, std::memory_order_relaxed);
    TEST_ASSERT_EQUAL_UINT (0, allocation_count.load (std::memory_order_relaxed));
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_create);
    RUN_TEST (test_check_read_empty);
    RUN_TEST (test_read_empty);
    RUN_TEST (test_write_complete_and_check_read_and_read);
    RUN_TEST (test_write_complete_and_flush_and_check_read_and_read);
    RUN_TEST (test_read_reports_prefetched_batch_tail);
    RUN_TEST (test_published_probe_does_not_put_empty_reader_to_sleep);
    RUN_TEST (test_read_if_distinguishes_empty_rejected_and_consumed);
    RUN_TEST (test_read_if_reports_prefetched_batch_tail);
    RUN_TEST (test_conflate_read_if_retains_or_consumes_under_one_decision);
    RUN_TEST (test_conflate_complete_records_topics_rollback_and_ownership);
    RUN_TEST (test_conflate_concurrent_multipart_and_wake);
    RUN_TEST (test_conflate_empty_reader_wake_exchange);
    RUN_TEST (test_conflate_publication_races_empty_observation);
    RUN_TEST (test_conflate_steady_state_does_not_allocate);
    RUN_TEST (test_conflate_burst_of_distinct_topics_releases_beyond_spare_cap);
    RUN_TEST (test_conflate_burst_cycles_within_spare_cap_do_not_allocate);
    RUN_TEST (test_conflate_reject_racing_replacement_keeps_position_and_charge);
    RUN_TEST (test_conflate_probe_racing_replacement_keeps_position_and_charge);
    RUN_TEST (test_conflate_racing_replacement_discard_returns_charge);
    RUN_TEST (test_conflate_distinct_topics_consumed_one_at_a_time_stay_bounded);
    RUN_TEST (test_conflate_two_racing_replacements_skip_two_stale_entries);
    RUN_TEST (test_conflate_mixed_reader_stress_conserves_charge);

    return UNITY_END ();
}
