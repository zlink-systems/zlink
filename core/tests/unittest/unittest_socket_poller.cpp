/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include "core/socket_poller.hpp"
#include "utils/ip.hpp"

#include <unity.h>

namespace
{
void test_single_fd_registration_reports_input ()
{
    zlink::signaler_t signaler;
    TEST_ASSERT_TRUE (signaler.valid ());

    zlink::socket_poller_t poller;
    int user_data = 7;
    TEST_ASSERT_SUCCESS_ERRNO (
      poller.add_fd (signaler.get_fd (), &user_data, ZLINK_POLLIN));

    signaler.send ();

    zlink::socket_poller_t::event_t event;
    TEST_ASSERT_EQUAL_INT (1, poller.wait (&event, 1, 1000));
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (signaler.get_fd ()),
                              static_cast<uint64_t> (event.fd));
    TEST_ASSERT_EQUAL_PTR (&user_data, event.user_data);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, event.events);

    signaler.recv ();
    TEST_ASSERT_SUCCESS_ERRNO (poller.remove_fd (signaler.get_fd ()));
}

void test_many_fd_registrations_survive_remove_and_rebuild ()
{
    const size_t registration_count = ZLINK_POLLITEMS_DFLT + 1;
    zlink::signaler_t signalers[registration_count];
    int user_data[registration_count];

    zlink::socket_poller_t poller;
    poller.reserve (registration_count);
    for (size_t i = 0; i < registration_count; ++i) {
        TEST_ASSERT_TRUE (signalers[i].valid ());
        user_data[i] = static_cast<int> (i);
        TEST_ASSERT_SUCCESS_ERRNO (
          poller.add_fd (signalers[i].get_fd (), &user_data[i], ZLINK_POLLIN));
    }

    const size_t removed = registration_count / 2;
    TEST_ASSERT_SUCCESS_ERRNO (
      poller.remove_fd (signalers[removed].get_fd ()));

    const size_t ready = registration_count - 1;
    signalers[ready].send ();

    zlink::socket_poller_t::event_t event;
    TEST_ASSERT_EQUAL_INT (1, poller.wait (&event, 1, 1000));
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (signalers[ready].get_fd ()),
                              static_cast<uint64_t> (event.fd));
    TEST_ASSERT_EQUAL_PTR (&user_data[ready], event.user_data);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, event.events);
    signalers[ready].recv ();

    for (size_t i = 0; i < registration_count; ++i) {
        if (i != removed)
            TEST_ASSERT_SUCCESS_ERRNO (
              poller.remove_fd (signalers[i].get_fd ()));
    }
}
} // namespace

int main ()
{
    UNITY_BEGIN ();
    zlink::initialize_network ();
    setup_test_environment (10);

    RUN_TEST (test_single_fd_registration_reports_input);
    RUN_TEST (test_many_fd_registrations_survive_remove_and_rebuild);

    zlink::shutdown_network ();
    return UNITY_END ();
}
