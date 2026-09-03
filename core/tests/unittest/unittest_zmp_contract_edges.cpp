/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include "api/message/submit_result_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_frame_buffer_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_completion_queue_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_pending_internal.hpp"
#include "core/transport_pair_policy.hpp"
#include "protocol/zmp_encoder.hpp"
#include "protocol/zmp_protocol.hpp"
#include "protocol/wire.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>

namespace
{
void test_completion_socket_buffer_preserves_default_and_caps_explicit_value ()
{
    using zlink::transport_pair_policy::completion_socket_buffer;
    using zlink::transport_pair_policy::completion_socket_buffer_bytes;

    TEST_ASSERT_EQUAL_INT (-1, completion_socket_buffer (-1));
    TEST_ASSERT_EQUAL_INT (32768, completion_socket_buffer (32768));
    TEST_ASSERT_EQUAL_INT (completion_socket_buffer_bytes,
                           completion_socket_buffer (1024 * 1024));
    TEST_ASSERT_EQUAL_UINT64 (
      32 * 1024 * 1024,
      zlink::transport_pair_policy::request_correlation_work_budget);
    TEST_ASSERT_EQUAL_UINT64 (
      1024,
      zlink::transport_pair_policy::request_correlation_work_charge (1024));
    TEST_ASSERT_EQUAL_UINT64 (
      65536,
      zlink::transport_pair_policy::request_correlation_work_charge (4096));
    TEST_ASSERT_EQUAL_UINT64 (
      524288,
      zlink::transport_pair_policy::request_correlation_work_charge (8192));
    TEST_ASSERT_EQUAL_UINT64 (
      32 * 1024 * 1024,
      zlink::transport_pair_policy::request_correlation_work_charge (32768));
    TEST_ASSERT_EQUAL_UINT64 (
      32 * 1024 * 1024 + 1,
      zlink::transport_pair_policy::request_correlation_work_charge (32769));
    TEST_ASSERT_EQUAL_UINT64 (
      32 * 1024 * 1024 + 1,
      zlink::transport_pair_policy::request_correlation_work_charge (
        UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT64 (
      16 * 1024,
      zlink::transport_pair_policy::request_correlation_count_budget);
}

void test_router_reply_peer_type_snapshot_is_part_of_alias_identity ()
{
    using namespace zlink::socket_reqrep_internal;

    router_reply_target_t dealer_target;
    dealer_target.source_peer_socket_type = ZLINK_CORE_SOCKET_DEALER;
    dealer_target.wire_request_seq = 17;
    dealer_target.transport_pair_id = 23;
    dealer_target.transport_pair_generation = 5;
    const router_reply_target_t copied_target = dealer_target;
    TEST_ASSERT_EQUAL_INT (ZLINK_CORE_SOCKET_DEALER,
                           copied_target.source_peer_socket_type);

    router_reply_alias_key_t dealer_alias;
    dealer_alias.source_peer_socket_type = ZLINK_CORE_SOCKET_DEALER;
    dealer_alias.wire_request_seq = dealer_target.wire_request_seq;
    dealer_alias.transport_pair_id = dealer_target.transport_pair_id;
    dealer_alias.transport_pair_generation =
      dealer_target.transport_pair_generation;
    router_reply_alias_key_t router_alias = dealer_alias;
    router_alias.source_peer_socket_type = ZLINK_CORE_SOCKET_ROUTER;

    TEST_ASSERT_FALSE (dealer_alias == router_alias);
    std::unordered_map<router_reply_alias_key_t, int,
                       router_reply_alias_key_hash_t>
      aliases;
    aliases.emplace (dealer_alias, 1);
    aliases.emplace (router_alias, 2);
    TEST_ASSERT_EQUAL_UINT64 (2, aliases.size ());
}

void test_router_reply_rid_owner_preserves_full_public_identity ()
{
    using namespace zlink::socket_reqrep_internal;

    unsigned char first_rid[255];
    unsigned char second_rid[255];
    memset (first_rid, 0xa5, sizeof (first_rid));
    memcpy (second_rid, first_rid, sizeof (second_rid));
    second_rid[sizeof (second_rid) - 1] = 0x5a;

    fixed_routing_id_key_t first;
    fixed_routing_id_key_t first_copy;
    fixed_routing_id_key_t second;
    first.assign (first_rid, sizeof (first_rid));
    first_copy.assign (first_rid, sizeof (first_rid));
    second.assign (second_rid, sizeof (second_rid));

    TEST_ASSERT_EQUAL_UINT64 (sizeof (first_rid), first.size ());
    TEST_ASSERT_TRUE (first == first_copy);
    TEST_ASSERT_FALSE (first == second);

    std::unordered_map<fixed_routing_id_key_t, size_t,
                       fixed_routing_id_key_hash_t>
      owners;
    ++owners.emplace (first, 0).first->second;
    ++owners.emplace (first_copy, 0).first->second;
    ++owners.emplace (second, 0).first->second;
    TEST_ASSERT_EQUAL_UINT64 (2, owners.size ());
    TEST_ASSERT_EQUAL_UINT64 (2, owners.find (first)->second);
    TEST_ASSERT_EQUAL_UINT64 (1, owners.find (second)->second);
}

void test_request_reply_intrusive_stores_spill_reuse_and_errno ()
{
    using namespace zlink::socket_reqrep_internal;

    pending_request_store_t pending_requests;
    for (uint64_t key = 1; key <= 64; ++key) {
        pending_request_t pending;
        pending.identity.request_seq = key;
        pending.identity.cookie = key;
        TEST_ASSERT_TRUE (
          pending_requests.emplace (key, std::move (pending)).second);
    }
    pending_request_t spill_pending;
    spill_pending.identity.request_seq = 65;
    spill_pending.identity.cookie = 65;
    errno = ENOMEM;
    std::pair<pending_request_store_t::iterator, bool> pending_spill =
      pending_requests.emplace (65, std::move (spill_pending));
    TEST_ASSERT_TRUE (pending_spill.second);
    TEST_ASSERT_EQUAL_INT (0, errno);
    TEST_ASSERT_EQUAL_UINT64 (65, pending_requests.size ());

    pending_request_t duplicate_pending;
    duplicate_pending.identity.request_seq = 65;
    duplicate_pending.identity.cookie = 66;
    errno = ENOMEM;
    const std::pair<pending_request_store_t::iterator, bool>
      pending_duplicate =
        pending_requests.emplace (65, std::move (duplicate_pending));
    TEST_ASSERT_FALSE (pending_duplicate.second);
    TEST_ASSERT_TRUE (pending_duplicate.first == pending_requests.end ());
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    pending_request_store_t::node_t *const pending_spill_node =
      &*pending_spill.first;
    pending_requests.erase (pending_spill.first);
    pending_request_t replacement_pending;
    replacement_pending.identity.request_seq = 66;
    replacement_pending.identity.cookie = 66;
    errno = ENOMEM;
    const std::pair<pending_request_store_t::iterator, bool>
      pending_replacement =
        pending_requests.emplace (66, std::move (replacement_pending));
    TEST_ASSERT_TRUE (pending_replacement.second);
    TEST_ASSERT_EQUAL_PTR (pending_spill_node, &*pending_replacement.first);
    TEST_ASSERT_EQUAL_INT (0, errno);
    TEST_ASSERT_EQUAL_UINT64 (65, pending_requests.size ());

    typedef reply_target_store_t<dealer_reply_target_t> dealer_store_t;
    dealer_store_t dealer_targets;
    for (uint64_t key = 1; key <= 64; ++key) {
        dealer_reply_target_t target;
        target.request_seq = key;
        TEST_ASSERT_TRUE (dealer_targets.emplace (key, target).second);
    }
    dealer_reply_target_t spill_dealer;
    spill_dealer.request_seq = 65;
    errno = ENOMEM;
    std::pair<dealer_store_t::iterator, bool> dealer_spill =
      dealer_targets.emplace (65, spill_dealer);
    TEST_ASSERT_TRUE (dealer_spill.second);
    TEST_ASSERT_EQUAL_INT (0, errno);
    TEST_ASSERT_EQUAL_UINT64 (65, dealer_targets.size ());

    dealer_reply_target_t duplicate_dealer;
    duplicate_dealer.request_seq = 650;
    errno = ENOMEM;
    const std::pair<dealer_store_t::iterator, bool> dealer_duplicate =
      dealer_targets.emplace (65, duplicate_dealer);
    TEST_ASSERT_FALSE (dealer_duplicate.second);
    TEST_ASSERT_TRUE (dealer_duplicate.first == dealer_targets.end ());
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    dealer_store_t::node_t *const dealer_spill_node = &*dealer_spill.first;
    dealer_targets.erase (dealer_spill.first);
    dealer_reply_target_t replacement_dealer;
    replacement_dealer.request_seq = 66;
    errno = ENOMEM;
    const std::pair<dealer_store_t::iterator, bool> dealer_replacement =
      dealer_targets.emplace (66, replacement_dealer);
    TEST_ASSERT_TRUE (dealer_replacement.second);
    TEST_ASSERT_EQUAL_PTR (dealer_spill_node, &*dealer_replacement.first);
    TEST_ASSERT_EQUAL_INT (0, errno);
    TEST_ASSERT_EQUAL_UINT64 (65, dealer_targets.size ());

    typedef reply_target_store_t<router_reply_target_t> router_store_t;
    router_store_t router_targets;
    for (uint64_t key = 1; key <= 64; ++key) {
        const unsigned char rid_byte = static_cast<unsigned char> (key);
        router_reply_target_t target;
        target.peer_rid.assign (&rid_byte, sizeof (rid_byte));
        target.wire_request_seq = key;
        TEST_ASSERT_TRUE (router_targets.emplace (key, target).second);
    }
    const unsigned char spill_rid = 65;
    router_reply_target_t spill_router;
    spill_router.peer_rid.assign (&spill_rid, sizeof (spill_rid));
    spill_router.wire_request_seq = 65;
    errno = ENOMEM;
    std::pair<router_store_t::iterator, bool> router_spill =
      router_targets.emplace (65, spill_router);
    TEST_ASSERT_TRUE (router_spill.second);
    TEST_ASSERT_EQUAL_INT (0, errno);
    TEST_ASSERT_EQUAL_UINT64 (65, router_targets.size ());

    router_reply_target_t duplicate_router;
    duplicate_router.wire_request_seq = 650;
    errno = ENOMEM;
    const std::pair<router_store_t::iterator, bool> router_duplicate =
      router_targets.emplace (65, duplicate_router);
    TEST_ASSERT_FALSE (router_duplicate.second);
    TEST_ASSERT_TRUE (router_duplicate.first == router_targets.end ());
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    router_store_t::node_t *const router_spill_node = &*router_spill.first;
    router_targets.erase (router_spill.first);
    const unsigned char replacement_rid = 66;
    router_reply_target_t replacement_router;
    replacement_router.peer_rid.assign (&replacement_rid,
                                        sizeof (replacement_rid));
    replacement_router.wire_request_seq = 66;
    errno = ENOMEM;
    const std::pair<router_store_t::iterator, bool> router_replacement =
      router_targets.emplace (66, replacement_router);
    TEST_ASSERT_TRUE (router_replacement.second);
    TEST_ASSERT_EQUAL_PTR (router_spill_node, &*router_replacement.first);
    TEST_ASSERT_EQUAL_INT (0, errno);
    TEST_ASSERT_EQUAL_UINT64 (65, router_targets.size ());
}

struct timeout_barrier_t
{
    timeout_barrier_t () : fired (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool fired;
};

struct blocking_timeout_remove_barrier_t
{
    blocking_timeout_remove_barrier_t () : entered (false), release (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool entered;
    bool release;
};

void signal_timeout_barrier (void *userdata_)
{
    timeout_barrier_t *barrier = static_cast<timeout_barrier_t *> (userdata_);
    std::lock_guard<std::mutex> lock (barrier->mutex);
    barrier->fired = true;
    barrier->cv.notify_all ();
}

void block_after_timeout_pending_remove (void *userdata_)
{
    blocking_timeout_remove_barrier_t *barrier =
      static_cast<blocking_timeout_remove_barrier_t *> (userdata_);
    std::unique_lock<std::mutex> lock (barrier->mutex);
    barrier->entered = true;
    barrier->cv.notify_all ();
    barrier->cv.wait (lock, [barrier] { return barrier->release; });
}

}

void setUp ()
{
}

void tearDown ()
{
}

void test_zmp_encoder_rejects_payload_larger_than_u32 ()
{
    if (std::numeric_limits<size_t>::max ()
        <= std::numeric_limits<uint32_t>::max ())
        TEST_IGNORE_MESSAGE ("size_t cannot represent an oversized ZMP payload");

    unsigned char borrowed = 0;
    zlink::msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_data (
      &borrowed,
      static_cast<size_t> (std::numeric_limits<uint32_t>::max ()) + 1u,
      NULL, NULL));

    zlink::zmp_encoder_t encoder (64);
    errno = 0;
    encoder.load_msg (&msg);
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);

    unsigned char *encoded = NULL;
    TEST_ASSERT_EQUAL_UINT64 (0, encoder.encode (&encoded, 0));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

void test_zmp_encoder_keeps_ordinary_data_header_at_eight_bytes ()
{
    zlink::msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_buffer ("abc", 3));

    zlink::zmp_encoder_t encoder (64);
    encoder.load_msg (&msg);
    unsigned char *encoded = NULL;
    const size_t encoded_size = encoder.encode (&encoded, 0);

    TEST_ASSERT_EQUAL_UINT64 (zlink::zmp_header_size + 3, encoded_size);
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_magic, encoded[0]);
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_version, encoded[1]);
    TEST_ASSERT_EQUAL_HEX8 (0, encoded[2]);
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_kind_data, encoded[3]);
    TEST_ASSERT_EQUAL_UINT32 (3, zlink::get_uint32 (encoded + 4));
    TEST_ASSERT_EQUAL_MEMORY ("abc", encoded + zlink::zmp_header_size, 3);
}

void test_zmp_encoder_writes_request_sequence_extension_big_endian ()
{
    zlink::msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_buffer ("abc", 3));
    msg.set_flags (zlink::msg_t::more);
    const uint64_t sequence = UINT64_C (0x0102030405060708);
    TEST_ASSERT_SUCCESS_ERRNO (msg.set_request_reply_metadata (
      zlink::zmp_kind_request, sequence));

    zlink::zmp_encoder_t encoder (64);
    encoder.load_msg (&msg);
    unsigned char *encoded = NULL;
    const size_t encoded_size = encoder.encode (&encoded, 0);

    static const unsigned char expected_header[] = {
      zlink::zmp_magic, zlink::zmp_version, zlink::zmp_flag_more,
      zlink::zmp_kind_request, 0x00, 0x00, 0x00, 0x03,
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    TEST_ASSERT_EQUAL_UINT64 (zlink::zmp_request_reply_header_size + 3,
                              encoded_size);
    TEST_ASSERT_EQUAL_MEMORY (expected_header, encoded,
                              sizeof (expected_header));
    TEST_ASSERT_EQUAL_MEMORY (
      "abc", encoded + zlink::zmp_request_reply_header_size, 3);
}

void test_zmp_encoder_rejects_request_metadata_on_special_frame ()
{
    zlink::msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_size (0));
    msg.set_flags (zlink::msg_t::routing_id);
    TEST_ASSERT_SUCCESS_ERRNO (msg.set_request_reply_metadata (
      zlink::zmp_kind_reply, 1));

    zlink::zmp_encoder_t encoder (64);
    errno = 0;
    encoder.load_msg (&msg);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    unsigned char *encoded = NULL;
    TEST_ASSERT_EQUAL_UINT64 (0, encoder.encode (&encoded, 0));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
}

void test_zmp_encoder_rejects_request_metadata_mid_multipart ()
{
    zlink::zmp_encoder_t encoder (64);

    zlink::msg_t first;
    TEST_ASSERT_SUCCESS_ERRNO (first.init_size (0));
    first.set_flags (zlink::msg_t::more);
    encoder.load_msg (&first);
    unsigned char *encoded = NULL;
    TEST_ASSERT_EQUAL_UINT64 (zlink::zmp_header_size,
                              encoder.encode (&encoded, 0));

    zlink::msg_t second;
    TEST_ASSERT_SUCCESS_ERRNO (second.init_size (0));
    TEST_ASSERT_SUCCESS_ERRNO (second.set_request_reply_metadata (
      zlink::zmp_kind_request, 2));
    errno = 0;
    encoder.load_msg (&second);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    encoded = NULL;
    TEST_ASSERT_EQUAL_UINT64 (0, encoder.encode (&encoded, 0));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
}

void test_zmp_socket_send_rejects_oversized_borrowed_payload ()
{
    if (std::numeric_limits<size_t>::max ()
        <= std::numeric_limits<uint32_t>::max ())
        TEST_IGNORE_MESSAGE ("size_t cannot represent an oversized ZMP payload");

    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    unsigned char borrowed = 0;
    zlink_msg_t msg;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_msg_init_data (
        &msg, &borrowed,
        static_cast<size_t> (std::numeric_limits<uint32_t>::max ()) + 1u,
        NULL, NULL));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, test_send_single_msg (&msg, dealer, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_error_reply_with_zero_errno_becomes_protocol_error ()
{
    zlink_msg_t parts[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], 4));
    memset (zlink_msg_data (&parts[0]), 0, 4);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], 7));
    memcpy (zlink_msg_data (&parts[1]), "payload", 7);

    int callback_errno = 0;
    zlink_msg_t *callback_parts = parts;
    size_t callback_part_count = 2;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::request_reply::decode_reply_completion (
        zlink::request_reply::error_reply_type, parts, 2, &callback_errno,
        &callback_parts, &callback_part_count));
    TEST_ASSERT_EQUAL_INT (EPROTO, callback_errno);
    TEST_ASSERT_NULL (callback_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, callback_part_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[0]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[1]));
}

void test_error_reply_without_errno_part_becomes_protocol_error ()
{
    int callback_errno = 0;
    zlink_msg_t *callback_parts = reinterpret_cast<zlink_msg_t *> (1);
    size_t callback_part_count = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::request_reply::decode_reply_completion (
        zlink::request_reply::error_reply_type, NULL, 0, &callback_errno,
        &callback_parts, &callback_part_count));
    TEST_ASSERT_EQUAL_INT (EPROTO, callback_errno);
    TEST_ASSERT_NULL (callback_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, callback_part_count);
}

void test_missing_completion_pipe_is_not_connected ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    socket_handle_t handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (handle.socket);

    zlink_msg_t frame;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&frame, 5));
    memcpy (zlink_msg_data (&frame), "reply", 5);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_reqrep_internal::send_completion_staged_frames (
            handle.socket, NULL, NULL, NULL, 0, &frame));
    TEST_ASSERT_EQUAL_INT (ENOTCONN, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink::submit_result_internal::from_errno (errno));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&frame));

    handle = socket_handle_t ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_request_reply_frame_buffer_spills_after_eight_owned_frames ()
{
    zlink::socket_reqrep_internal::request_reply_frame_buffer_t frames;
    zlink_msg_t *inline_data = NULL;

    for (size_t i = 0; i < 9; ++i) {
        frames.append_uninitialized ();
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&frames.back (), 80 + i));
        memset (zlink_msg_data (&frames.back ()), static_cast<int> ('a' + i),
                zlink_msg_size (&frames.back ()));

        if (i == 0)
            inline_data = frames.data ();
        if (i < zlink::socket_reqrep_internal::inline_request_reply_frame_capacity)
            TEST_ASSERT_EQUAL_PTR (inline_data, frames.data ());
    }

    TEST_ASSERT_TRUE (frames.data () != inline_data);
    TEST_ASSERT_EQUAL_UINT64 (9, frames.size ());
    for (size_t i = 0; i < frames.size (); ++i) {
        TEST_ASSERT_EQUAL_UINT64 (80 + i, zlink_msg_size (&frames[i]));
        const unsigned char *data =
          static_cast<const unsigned char *> (zlink_msg_data (&frames[i]));
        TEST_ASSERT_EQUAL_HEX8 (static_cast<unsigned char> ('a' + i), data[0]);
        TEST_ASSERT_EQUAL_HEX8 (static_cast<unsigned char> ('a' + i),
                                data[zlink_msg_size (&frames[i]) - 1]);
    }

    zlink::socket_reqrep_internal::close_request_reply_frame_buffer (&frames);
    TEST_ASSERT_TRUE (frames.empty ());
}

void test_recv_sequence_buffers_two_parts_inline_and_rolls_back_oom ()
{
    using namespace zlink::part_helper_internal;
    std::shared_ptr<handle_state_t> state (new handle_state_t ());

    zlink_msg_t input[2];
    for (size_t i = 0; i < 2; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&input[i], 4));
        memset (zlink_msg_data (&input[i]), static_cast<int> ('a' + i), 4);
    }
    TEST_ASSERT_SUCCESS_ERRNO (stage_recv_sequence (
      state, recv_family_basic, NULL, NULL, 0, input, 2,
      std::this_thread::get_id ()));
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_TRUE (state->recv.active);
        TEST_ASSERT_EQUAL_UINT64 (2, state->recv.buffered_parts.size ());
        TEST_ASSERT_TRUE (state->recv.buffered_parts.capacity ()
                          >= inline_recv_part_capacity);
    }

    zlink_msg_t output;
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&output));
    TEST_ASSERT_SUCCESS_ERRNO (take_recv_part (state, &output, &more));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&output));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&output));
    TEST_ASSERT_SUCCESS_ERRNO (take_recv_part (state, &output, &more));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&output));
    complete_recv_step (state, more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&input[0]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&input[1]));

    zlink_msg_t retained;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&retained, 3));
    memcpy (zlink_msg_data (&retained), "oom", 3);
    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    source_rid.size = 3;
    memcpy (source_rid.data, "rid", source_rid.size);
    state->recv.transport_pair_id = 41;
    state->recv.transport_pair_generation = 42;
    const size_t impossible_part_count = std::numeric_limits<size_t>::max ();
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, stage_recv_sequence (
            state, recv_family_basic, NULL, &source_rid, 77, &retained,
            impossible_part_count, std::this_thread::get_id ()));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_FALSE (state->recv.active);
        TEST_ASSERT_EQUAL_INT (recv_family_none, state->recv.family);
        TEST_ASSERT_TRUE (state->recv.buffered_parts.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, state->recv.next_part_index);
        TEST_ASSERT_TRUE (state->recv.return_source_rid_as_null);
        TEST_ASSERT_EQUAL_UINT64 (0, state->recv.source_node_rid.size);
        TEST_ASSERT_EQUAL_UINT64 (0, state->recv.request_seq);
        TEST_ASSERT_EQUAL_UINT64 (0, state->recv.transport_pair_id);
        TEST_ASSERT_EQUAL_UINT64 (0,
                                  state->recv.transport_pair_generation);
    }
    TEST_ASSERT_EQUAL_UINT64 (3, zlink_msg_size (&retained));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&retained));
}

void test_zero_request_timeout_resolves_to_implementation_default ()
{
    TEST_ASSERT_EQUAL_UINT32 (
      zlink::request_reply::default_timeout_ms,
      zlink::request_reply::resolve_timeout_ms (0, 0));
    TEST_ASSERT_EQUAL_UINT32 (321,
                              zlink::request_reply::resolve_timeout_ms (0, 321));
    TEST_ASSERT_EQUAL_UINT32 (
      654, zlink::request_reply::resolve_timeout_ms (0, 321, 654));
    TEST_ASSERT_EQUAL_UINT32 (
      987, zlink::request_reply::resolve_timeout_ms (987, 321, 654));
}

void test_pending_aggregate_wrap_and_stale_cookie_are_fenced ()
{
    using namespace zlink::socket_reqrep_internal;
    std::shared_ptr<socket_request_reply_state_t> state (
      new socket_request_reply_state_t (NULL, ZLINK_CORE_SOCKET_ROUTER));

    pending_request_t wrap_blocker = pending_request_t ();
    wrap_blocker.identity.request_seq = std::numeric_limits<uint64_t>::max ();
    wrap_blocker.identity.cookie = 1;
    const pending_request_identity_t wrap_identity = wrap_blocker.identity;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), std::move (wrap_blocker)));
    state->next_request_seq = std::numeric_limits<uint64_t>::max ();
    TEST_ASSERT_EQUAL_UINT64 (
      1, zlink::request_reply_runtime::allocate_request_sequence (state.get ()));
    TEST_ASSERT_EQUAL_UINT64 (2, state->next_request_seq);

    pending_request_t removed = pending_request_t ();
    TEST_ASSERT_TRUE (remove_socket_pending_request_locked (
      state.get (), wrap_identity, &removed));

    pending_request_t old_request = pending_request_t ();
    old_request.identity.request_seq = 1;
    old_request.identity.cookie = 41;
    old_request.transport_pair_id = 0;
    old_request.transport_pair_generation = 0;
    pending_request_token_t stale_token;
    stale_token.identity = old_request.identity;
    stale_token.resolved_timeout_ms = 1000;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), std::move (old_request)));
    TEST_ASSERT_TRUE (
      remove_socket_pending_request_locked (state.get (), stale_token.identity, &removed));

    pending_request_t reused_request = pending_request_t ();
    reused_request.identity.request_seq = stale_token.identity.request_seq;
    reused_request.identity.cookie = 42;
    reused_request.transport_pair_id = 0;
    reused_request.transport_pair_generation = 0;
    const pending_request_identity_t reused_identity = reused_request.identity;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), std::move (reused_request)));

    TEST_ASSERT_FALSE (
      remove_socket_pending_request_locked (state.get (), stale_token.identity, &removed));
    TEST_ASSERT_FALSE (record_socket_pending_transport_pair_identity (
      state, stale_token.identity, 101, 9));
    TEST_ASSERT_SUCCESS_ERRNO (arm_socket_pending_request_timeout (state, stale_token));
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        const pending_request_store_t::const_iterator current =
          state->pending_requests.find (stale_token.identity.request_seq);
        TEST_ASSERT_TRUE (current != state->pending_requests.end ());
        TEST_ASSERT_EQUAL_UINT64 (reused_identity.cookie,
                                  current->second.identity.cookie);
        TEST_ASSERT_EQUAL_UINT64 (0, current->second.transport_pair_id);
        TEST_ASSERT_EQUAL_UINT64 (0,
                                  current->second.timeout_deadline_ns);
        TEST_ASSERT_FALSE (state->pending_timeout_task);
    }

    std::shared_ptr<zlink::request_timeout::task_t> stale_timeout;
    TEST_ASSERT_SUCCESS_ERRNO (schedule_socket_pending_timeout (
      state, stale_token.identity, 1, &stale_timeout));
    timeout_barrier_t barrier;
    std::shared_ptr<zlink::request_timeout::task_t> barrier_task =
      zlink::request_timeout::schedule (20, &signal_timeout_barrier, &barrier);
    TEST_ASSERT_NOT_NULL (barrier_task.get ());
    {
        std::unique_lock<std::mutex> lock (barrier.mutex);
        TEST_ASSERT_TRUE (barrier.cv.wait_for (
          lock, std::chrono::seconds (3), [&barrier] { return barrier.fired; }));
    }
    zlink::request_timeout::cancel (stale_timeout);
    zlink::request_timeout::cancel (barrier_task);

    TEST_ASSERT_EQUAL_UINT64 (1, state->pending_requests.size ());
    TEST_ASSERT_TRUE (remove_socket_pending_request_locked (
      state.get (), reused_request.identity, &removed));
    TEST_ASSERT_TRUE (state->pending_requests.empty ());
}

void test_pending_timeout_task_is_reused_and_only_replaced_for_earlier_deadline ()
{
    using namespace zlink::socket_reqrep_internal;
    std::shared_ptr<socket_request_reply_state_t> state (
      new socket_request_reply_state_t (NULL, ZLINK_CORE_SOCKET_DEALER));

    pending_request_t first;
    first.identity.request_seq = 1;
    first.identity.cookie = 1;
    pending_request_token_t first_token;
    first_token.identity = first.identity;
    first_token.resolved_timeout_ms = 20000;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), std::move (first)));
    TEST_ASSERT_SUCCESS_ERRNO (
      arm_socket_pending_request_timeout (state, first_token));
    std::shared_ptr<zlink::request_timeout::task_t> first_task;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        first_task = state->pending_timeout_task;
        TEST_ASSERT_NOT_NULL (first_task.get ());
        TEST_ASSERT_NOT_EQUAL (
          0, state->pending_requests.find (1)->second.timeout_deadline_ns);
    }

    pending_request_t removed;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_TRUE (remove_socket_pending_request_locked (
          state.get (), first_token.identity, &removed));
    }

    pending_request_t later;
    later.identity.request_seq = 2;
    later.identity.cookie = 2;
    pending_request_token_t later_token;
    later_token.identity = later.identity;
    later_token.resolved_timeout_ms = 30000;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), std::move (later)));
    TEST_ASSERT_SUCCESS_ERRNO (
      arm_socket_pending_request_timeout (state, later_token));
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_EQUAL_PTR (first_task.get (),
                               state->pending_timeout_task.get ());
        TEST_ASSERT_TRUE (remove_socket_pending_request_locked (
          state.get (), later_token.identity, &removed));
    }

    pending_request_t earlier;
    earlier.identity.request_seq = 3;
    earlier.identity.cookie = 3;
    pending_request_token_t earlier_token;
    earlier_token.identity = earlier.identity;
    earlier_token.resolved_timeout_ms = 10000;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), std::move (earlier)));
    TEST_ASSERT_SUCCESS_ERRNO (
      arm_socket_pending_request_timeout (state, earlier_token));
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_NOT_NULL (state->pending_timeout_task.get ());
        TEST_ASSERT_TRUE (first_task.get ()
                          != state->pending_timeout_task.get ());
        TEST_ASSERT_TRUE (remove_socket_pending_request_locked (
          state.get (), earlier_token.identity, &removed));
    }

    cancel_socket_pending_timeouts (state);
    TEST_ASSERT_FALSE (state->pending_timeout_task);
}

void test_suspended_request_multipart_preserves_pending_cookie ()
{
    using namespace zlink::part_helper_internal;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    socket_handle_t handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (handle.socket);

    send_sequence_spec_t staged;
    staged.family = send_family_dealer_request;
    staged.request_like = true;
    std::shared_ptr<handle_state_t> helper_state;
    bool first_part = false;
    TEST_ASSERT_SUCCESS_ERRNO (prepare_send_step (
      dealer, staged, handle.socket, &helper_state, &first_part));
    TEST_ASSERT_TRUE (first_part);
    complete_send_step (helper_state, ZLINK_PART_MORE);

    send_sequence_spec_t resumed = staged;
    resumed.timeout_ms = 1000;
    resumed.request_seq = 77;
    resumed.pending_cookie = 991;
    TEST_ASSERT_SUCCESS_ERRNO (prepare_send_step (
      dealer, resumed, handle.socket, &helper_state, &first_part));
    TEST_ASSERT_FALSE (first_part);
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (resumed.request_seq,
                                  helper_state->send.spec.request_seq);
        TEST_ASSERT_EQUAL_UINT64 (resumed.pending_cookie,
                                  helper_state->send.spec.pending_cookie);
    }
    abort_send_step (helper_state);

    handle = socket_handle_t ();
    helper_state.reset ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pending_cookie_wrap_skips_zero ()
{
    using namespace zlink::socket_reqrep_internal;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    socket_handle_t handle = as_socket_handle (dealer);
    std::shared_ptr<socket_request_reply_state_t> state =
      find_or_create_request_reply_state (handle);
    TEST_ASSERT_NOT_NULL (state.get ());
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->next_pending_cookie = std::numeric_limits<uint64_t>::max ();
    }

    uint64_t request_seq = 0;
    std::shared_ptr<socket_request_reply_state_t> registered_state;
    pending_request_token_t token;
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (ensure_socket_pull_pending_request (
      handle, 1000, NULL, NULL, &request_seq, &registered_state, &token,
      &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    TEST_ASSERT_EQUAL_UINT64 (request_seq, token.identity.request_seq);
    TEST_ASSERT_EQUAL_UINT64 (std::numeric_limits<uint64_t>::max (),
                              token.identity.cookie);
    TEST_ASSERT_TRUE (erase_socket_pending_request (state, token.identity));

    request_seq = 0;
    completion_id = 0;
    registered_state.reset ();
    token = pending_request_token_t ();
    TEST_ASSERT_SUCCESS_ERRNO (ensure_socket_pull_pending_request (
      handle, 1000, NULL, NULL, &request_seq, &registered_state, &token,
      &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    TEST_ASSERT_EQUAL_UINT64 (request_seq, token.identity.request_seq);
    TEST_ASSERT_EQUAL_UINT64 (1, token.identity.cookie);
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (2, state->next_pending_cookie);
    }
    TEST_ASSERT_TRUE (erase_socket_pending_request (state, token.identity));

    handle = socket_handle_t ();
    registered_state.reset ();
    state.reset ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pending_insert_failure_releases_completion_reservation ()
{
    using namespace zlink::socket_reqrep_internal;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    socket_handle_t handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (handle.socket);
    std::shared_ptr<socket_request_reply_state_t> state =
      find_or_create_request_reply_state (handle);
    TEST_ASSERT_NOT_NULL (state.get ());

    test_set_request_reply_allocation_failpoint (
      request_reply_allocation_pending_insert);
    uint64_t request_seq = 0;
    std::shared_ptr<socket_request_reply_state_t> registered_state;
    pending_request_token_t token;
    zlink_completion_id_t completion_id = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, ensure_socket_pull_pending_request (
            handle, 1000, NULL, NULL, &request_seq, &registered_state,
            &token, &completion_id));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    TEST_ASSERT_NULL (registered_state.get ());
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_TRUE (state->pending_requests.empty ());
    }
    TEST_ASSERT_EQUAL_UINT64 (
      0, zlink::socket_completion::outstanding (
           &handle.socket->completion_runtime ()));
    zlink::socket_completion::reservation_t *reservation = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &handle.socket->completion_runtime (), ZLINK_COMPLETION_REQUEST, NULL,
      NULL, &reservation, &completion_id));
    TEST_ASSERT_NOT_NULL (reservation);
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    zlink::socket_completion::release (&handle.socket->completion_runtime (),
                                       reservation);

    handle = socket_handle_t ();
    state.reset ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_timeout_remove_can_race_close_without_post_close_delivery ()
{
    using namespace zlink::socket_reqrep_internal;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    socket_handle_t handle = as_socket_handle (dealer);
    std::shared_ptr<socket_request_reply_state_t> state =
      find_or_create_request_reply_state (handle);
    TEST_ASSERT_NOT_NULL (state.get ());

    uint64_t request_seq = 0;
    std::shared_ptr<socket_request_reply_state_t> registered_state;
    pending_request_token_t token;
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (ensure_socket_pull_pending_request (
      handle, 20, NULL, NULL, &request_seq, &registered_state,
      &token, &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);

    blocking_timeout_remove_barrier_t barrier;
    test_set_request_reply_timeout_after_remove_hook (
      &block_after_timeout_pending_remove, &barrier);
    TEST_ASSERT_SUCCESS_ERRNO (
      arm_socket_pending_request_timeout (state, token));
    handle = socket_handle_t ();

    {
        std::unique_lock<std::mutex> lock (barrier.mutex);
        TEST_ASSERT_TRUE (barrier.cv.wait_for (
          lock, std::chrono::seconds (3), [&barrier] { return barrier.entered; }));
    }

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));

    {
        std::lock_guard<std::mutex> lock (barrier.mutex);
        barrier.release = true;
    }
    barrier.cv.notify_all ();

    registered_state.reset ();
    state.reset ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
    test_set_request_reply_timeout_after_remove_hook (NULL, NULL);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (
      test_completion_socket_buffer_preserves_default_and_caps_explicit_value);
    RUN_TEST (
      test_router_reply_peer_type_snapshot_is_part_of_alias_identity);
    RUN_TEST (test_router_reply_rid_owner_preserves_full_public_identity);
    RUN_TEST (test_request_reply_intrusive_stores_spill_reuse_and_errno);
    RUN_TEST (test_zmp_encoder_rejects_payload_larger_than_u32);
    RUN_TEST (test_zmp_encoder_keeps_ordinary_data_header_at_eight_bytes);
    RUN_TEST (test_zmp_encoder_writes_request_sequence_extension_big_endian);
    RUN_TEST (test_zmp_encoder_rejects_request_metadata_on_special_frame);
    RUN_TEST (test_zmp_encoder_rejects_request_metadata_mid_multipart);
    RUN_TEST (test_zmp_socket_send_rejects_oversized_borrowed_payload);
    RUN_TEST (test_error_reply_with_zero_errno_becomes_protocol_error);
    RUN_TEST (test_error_reply_without_errno_part_becomes_protocol_error);
    RUN_TEST (test_missing_completion_pipe_is_not_connected);
    RUN_TEST (test_request_reply_frame_buffer_spills_after_eight_owned_frames);
    RUN_TEST (test_recv_sequence_buffers_two_parts_inline_and_rolls_back_oom);
    RUN_TEST (test_zero_request_timeout_resolves_to_implementation_default);
    RUN_TEST (test_pending_aggregate_wrap_and_stale_cookie_are_fenced);
    RUN_TEST (
      test_pending_timeout_task_is_reused_and_only_replaced_for_earlier_deadline);
    RUN_TEST (test_suspended_request_multipart_preserves_pending_cookie);
    RUN_TEST (test_pending_cookie_wrap_skips_zero);
    RUN_TEST (test_pending_insert_failure_releases_completion_reservation);
    RUN_TEST (test_timeout_remove_can_race_close_without_post_close_delivery);
    return UNITY_END ();
}
