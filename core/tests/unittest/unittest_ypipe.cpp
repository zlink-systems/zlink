/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include <ypipe.hpp>
#include <ypipe_conflate.hpp>

#include <unity.h>

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
        read_conflate_frame (pipe, '1', false);
        read_conflate_frame (pipe, 'A', true);
        read_conflate_frame (pipe, '3', false);
        read_conflate_frame (pipe, 'B', true);
        read_conflate_frame (pipe, '4', false);
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

    return UNITY_END ();
}
