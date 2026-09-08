/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#include <boost/beast/websocket/detail/mask.hpp>
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS && defined ZLINK_HAVE_WS
#include "transports/ws/ws_transport.hpp"
#if defined ZLINK_HAVE_WSS
#include "transports/tls/wss_transport.hpp"
#endif
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

void setUp () {}
void tearDown () {}

namespace {

using boost::asio::mutable_buffer;
using boost::beast::websocket::detail::mask_inplace;
using boost::beast::websocket::detail::prepared_key;

void reference_mask_chunk (std::vector<unsigned char> &bytes,
                           size_t offset,
                           size_t length,
                           prepared_key &key)
{
    const prepared_key original_key = key;
    for (size_t i = 0; i != length; ++i)
        bytes[offset + i] ^= original_key[i % original_key.size ()];

    const size_t rotation = length % original_key.size ();
    for (size_t i = 0; i != original_key.size (); ++i)
        key[i] = original_key[(i + rotation) % original_key.size ()];
}

void mask_in_chunks (std::vector<unsigned char> &bytes,
                     size_t offset,
                     size_t length,
                     prepared_key &key)
{
    const size_t prefix[] = {1, 3, 4, 7};
    size_t position = 0;
    for (size_t i = 0; i != sizeof (prefix) / sizeof (prefix[0]); ++i) {
        size_t chunk = prefix[i];
        if (chunk > length - position)
            chunk = length - position;
        if (chunk != 0)
            mask_inplace (
              mutable_buffer (bytes.data () + offset + position, chunk), key);
        position += chunk;
    }
    if (position != length)
        mask_inplace (mutable_buffer (bytes.data () + offset + position,
                                      length - position),
                      key);
}

} // namespace

void test_ws_mask_inplace_matches_byte_reference ()
{
    const size_t near_64k[] = {65535, 65536, 65537};
    const std::uint32_t key_value = 0x44332211;

    for (size_t length_case = 0; length_case != 35; ++length_case) {
        const size_t length = length_case < 32 ? length_case
                                               : near_64k[length_case - 32];
        for (size_t alignment = 0; alignment != 8; ++alignment) {
            std::vector<unsigned char> original (length + 8, 0xa5);
            for (size_t i = 0; i != length; ++i)
                original[alignment + i] = static_cast<unsigned char> (
                  (i * 29 + length * 7 + alignment * 13) & 0xff);

            for (size_t initial_rotation = 0; initial_rotation != 4;
                 ++initial_rotation) {
                prepared_key base_key;
                boost::beast::websocket::detail::prepare_key (base_key,
                                                               key_value);
                prepared_key initial_key;
                for (size_t i = 0; i != initial_key.size (); ++i)
                    initial_key[i] =
                      base_key[(i + initial_rotation) % base_key.size ()];

                std::vector<unsigned char> storage = original;
                prepared_key actual_key = initial_key;
                mask_inplace (mutable_buffer (storage.data () + alignment,
                                              length),
                              actual_key);

                std::vector<unsigned char> expected = original;
                prepared_key expected_key = initial_key;
                reference_mask_chunk (expected, alignment, length, expected_key);
                TEST_ASSERT_EQUAL_MEMORY (expected.data (),
                                          storage.data (), original.size ());
                TEST_ASSERT_EQUAL_MEMORY (expected_key.data (),
                                          actual_key.data (),
                                          expected_key.size ());

                prepared_key restore_key = initial_key;
                mask_inplace (mutable_buffer (storage.data () + alignment,
                                              length),
                              restore_key);
                TEST_ASSERT_EQUAL_MEMORY (original.data (),
                                          storage.data (), original.size ());
                TEST_ASSERT_EQUAL_MEMORY (expected_key.data (),
                                          restore_key.data (),
                                          expected_key.size ());

                storage = original;
                actual_key = initial_key;
                mask_in_chunks (storage, alignment, length, actual_key);

                expected = original;
                expected_key = initial_key;
                const size_t prefix[] = {1, 3, 4, 7};
                size_t position = 0;
                for (size_t i = 0;
                     i != sizeof (prefix) / sizeof (prefix[0]); ++i) {
                    size_t chunk = prefix[i];
                    if (chunk > length - position)
                        chunk = length - position;
                    if (chunk != 0)
                        reference_mask_chunk (expected, alignment + position, chunk,
                                              expected_key);
                    position += chunk;
                }
                if (position != length)
                    reference_mask_chunk (expected, alignment + position,
                                         length - position, expected_key);
                TEST_ASSERT_EQUAL_MEMORY (
                  expected.data (), storage.data (), original.size ());
                TEST_ASSERT_EQUAL_MEMORY (expected_key.data (),
                                          actual_key.data (),
                                          expected_key.size ());

                restore_key = initial_key;
                mask_in_chunks (storage, alignment, length, restore_key);
                TEST_ASSERT_EQUAL_MEMORY (original.data (),
                                          storage.data (), original.size ());
                TEST_ASSERT_EQUAL_MEMORY (expected_key.data (),
                                          restore_key.data (),
                                          expected_key.size ());
            }
        }
    }
}

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS && defined ZLINK_HAVE_WS
void test_ws_transport_config_initialization_is_thread_safe ()
{
    const size_t worker_count = 16;
    std::atomic<size_t> ready (0);
    std::atomic<bool> start (false);
    std::vector<std::array<size_t, 4> > values (worker_count);
    std::vector<std::thread> workers;
    workers.reserve (worker_count);

    for (size_t i = 0; i != worker_count; ++i) {
        workers.push_back (std::thread ([&, i] () {
            ready.fetch_add (1, std::memory_order_release);
            while (!start.load (std::memory_order_acquire))
                std::this_thread::yield ();

            values[i][0] = zlink::test_ws_write_buffer_bytes ();
            values[i][1] = zlink::test_ws_read_message_max ();
#if defined ZLINK_HAVE_WSS
            values[i][2] = zlink::test_wss_write_buffer_bytes ();
            values[i][3] = zlink::test_wss_read_message_max ();
#else
            values[i][2] = 0;
            values[i][3] = 0;
#endif
        }));
    }

    while (ready.load (std::memory_order_acquire) != worker_count)
        std::this_thread::yield ();
    start.store (true, std::memory_order_release);

    for (size_t i = 0; i != workers.size (); ++i)
        workers[i].join ();

    TEST_ASSERT_GREATER_THAN_UINT64 (0, values[0][0]);
    TEST_ASSERT_GREATER_THAN_UINT64 (0, values[0][1]);
    for (size_t i = 1; i != worker_count; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (values[0][0], values[i][0]);
        TEST_ASSERT_EQUAL_UINT64 (values[0][1], values[i][1]);
#if defined ZLINK_HAVE_WSS
        TEST_ASSERT_EQUAL_UINT64 (values[0][2], values[i][2]);
        TEST_ASSERT_EQUAL_UINT64 (values[0][3], values[i][3]);
#endif
    }
}

#endif
int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_ws_mask_inplace_matches_byte_reference);
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS && defined ZLINK_HAVE_WS
    RUN_TEST (test_ws_transport_config_initialization_is_thread_safe);
#endif
    return UNITY_END ();
}
