/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "platform.hpp"
#include "transports/ipc/ipc_address.hpp"

#include <unity.h>

#if defined ZLINK_HAVE_IPC

#include <cstddef>
#include <cstring>
#include <string>

void setUp ()
{
}

void tearDown ()
{
}

void test_default_ipc_address_has_no_string ()
{
    zlink::ipc_address_t address;
    std::string text = "unchanged";

    TEST_ASSERT_EQUAL_INT (-1, address.to_string (text));
    TEST_ASSERT_TRUE (text.empty ());
    TEST_ASSERT_EQUAL_UINT (0, (unsigned int) address.addrlen ());
}

void test_resolve_round_trips_path ()
{
    zlink::ipc_address_t address;
    std::string text;

    TEST_ASSERT_EQUAL_INT (0, address.resolve ("/tmp/zlink-ipc-address-test"));
    TEST_ASSERT_EQUAL_INT (0, address.to_string (text));
    TEST_ASSERT_EQUAL_STRING ("ipc:///tmp/zlink-ipc-address-test", text.c_str ());
}

void test_resolve_round_trips_abstract_path ()
{
    zlink::ipc_address_t address;
    std::string text;

    TEST_ASSERT_EQUAL_INT (0, address.resolve ("@zlink-ipc-address-test"));
    TEST_ASSERT_EQUAL_INT (0, address.to_string (text));
    TEST_ASSERT_EQUAL_STRING ("ipc://@zlink-ipc-address-test", text.c_str ());
}

void test_sockaddr_with_no_path_returns_prefix_only ()
{
    sockaddr_un raw;
    std::memset (&raw, 0, sizeof raw);
    raw.sun_family = AF_UNIX;

    zlink::ipc_address_t address (reinterpret_cast<const sockaddr *> (&raw),
                                  static_cast<socklen_t> (offsetof (sockaddr_un, sun_path)));
    std::string text;

    TEST_ASSERT_EQUAL_INT (0, address.to_string (text));
    TEST_ASSERT_EQUAL_STRING ("ipc://", text.c_str ());
}

void test_sockaddr_length_is_clamped_to_storage ()
{
    sockaddr_un raw;
    std::memset (&raw, 0, sizeof raw);
    raw.sun_family = AF_UNIX;
    std::memset (raw.sun_path, 'x', sizeof raw.sun_path);

    zlink::ipc_address_t address (reinterpret_cast<const sockaddr *> (&raw),
                                  static_cast<socklen_t> (sizeof raw + 32));
    std::string text;

    TEST_ASSERT_EQUAL_INT (0, address.to_string (text));
    TEST_ASSERT_EQUAL_UINT (sizeof ("ipc://") - 1 + sizeof raw.sun_path,
                            (unsigned int) text.size ());
}

void test_short_abstract_sockaddr_does_not_read_past_addrlen ()
{
    sockaddr_un raw;
    std::memset (&raw, 0, sizeof raw);
    raw.sun_family = AF_UNIX;
    raw.sun_path[1] = 'a';

    zlink::ipc_address_t address (reinterpret_cast<const sockaddr *> (&raw),
                                  static_cast<socklen_t> (offsetof (sockaddr_un, sun_path) + 1));
    std::string text;

    TEST_ASSERT_EQUAL_INT (0, address.to_string (text));
    TEST_ASSERT_EQUAL_STRING ("ipc://", text.c_str ());
}

void test_abstract_sockaddr_keeps_at_prefix_when_path_is_present ()
{
    sockaddr_un raw;
    std::memset (&raw, 0, sizeof raw);
    raw.sun_family = AF_UNIX;
    raw.sun_path[1] = 'a';

    zlink::ipc_address_t address (reinterpret_cast<const sockaddr *> (&raw),
                                  static_cast<socklen_t> (offsetof (sockaddr_un, sun_path) + 2));
    std::string text;

    TEST_ASSERT_EQUAL_INT (0, address.to_string (text));
    TEST_ASSERT_EQUAL_STRING ("ipc://@a", text.c_str ());
}

#endif

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

#if defined ZLINK_HAVE_IPC
    RUN_TEST (test_default_ipc_address_has_no_string);
    RUN_TEST (test_resolve_round_trips_path);
    RUN_TEST (test_resolve_round_trips_abstract_path);
    RUN_TEST (test_sockaddr_with_no_path_returns_prefix_only);
    RUN_TEST (test_sockaddr_length_is_clamped_to_storage);
    RUN_TEST (test_short_abstract_sockaddr_does_not_read_past_addrlen);
    RUN_TEST (test_abstract_sockaddr_keeps_at_prefix_when_path_is_present);
#endif

    return UNITY_END ();
}
