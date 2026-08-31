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

void test_read_if_distinguishes_empty_rejected_and_consumed ()
{
    zlink::ypipe_t<int, 1> ypipe;
    int read_value = -1;
    int_read_context_t context = {42, false, 0};

    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_empty,
      ypipe.read_if (&read_value, &read_int_if, &context));
    TEST_ASSERT_EQUAL_INT (0, context.calls);
    TEST_ASSERT_EQUAL_INT (-1, read_value);

    ypipe.write (context.expected, false);
    ypipe.flush ();
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_rejected,
      ypipe.read_if (&read_value, &read_int_if, &context));
    TEST_ASSERT_EQUAL_INT (1, context.calls);
    TEST_ASSERT_EQUAL_INT (-1, read_value);
    TEST_ASSERT_TRUE (ypipe.check_read ());

    context.accept = true;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_consumed,
      ypipe.read_if (&read_value, &read_int_if, &context));
    TEST_ASSERT_EQUAL_INT (2, context.calls);
    TEST_ASSERT_EQUAL_INT (context.expected, read_value);
    TEST_ASSERT_FALSE (ypipe.check_read ());
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
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_rejected,
      ypipe.read_if (&read_value, &read_msg_if, &context));
    TEST_ASSERT_EQUAL_INT (1, context.calls);
    TEST_ASSERT_EQUAL_UINT (0, read_value.size ());

    context.accept = true;
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_consumed,
      ypipe.read_if (&read_value, &read_msg_if, &context));
    TEST_ASSERT_EQUAL_INT (2, context.calls);
    TEST_ASSERT_EQUAL_HEX8 (
      context.expected,
      *static_cast<unsigned char *> (read_value.data ()));
    TEST_ASSERT_EQUAL_INT (0, read_value.close ());

    TEST_ASSERT_EQUAL_INT (0, read_value.init ());
    TEST_ASSERT_EQUAL_INT (
      zlink::ypipe_read_empty,
      ypipe.read_if (&read_value, &read_msg_if, &context));
    TEST_ASSERT_EQUAL_INT (2, context.calls);
    TEST_ASSERT_EQUAL_INT (0, read_value.close ());

    TEST_ASSERT_EQUAL_INT (0, first.close ());
    TEST_ASSERT_EQUAL_INT (0, replacement.close ());
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
    RUN_TEST (test_read_if_distinguishes_empty_rejected_and_consumed);
    RUN_TEST (test_conflate_read_if_retains_or_consumes_under_one_decision);

    return UNITY_END ();
}
