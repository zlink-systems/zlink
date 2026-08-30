/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "core/msg.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <unity.h>

static_assert (sizeof (zlink::msg_t) == 64,
               "msg_t ABI size must remain unchanged");
static_assert (zlink::msg_t::max_vsm_size == 29,
               "msg_t inline payload capacity must remain unchanged");
static_assert (std::is_trivially_copyable<zlink::msg_t>::value,
               "msg_t must remain safe for existing raw relocation paths");

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
const unsigned char request_kind = 1;
const unsigned char reply_kind = 2;
const uint64_t request_sequence = UINT64_C (0x0102030405060708);

void ignore_external_free (void *, void *)
{
}

void assert_no_request_reply_metadata (const zlink::msg_t &msg_)
{
    unsigned char kind = 0xff;
    uint64_t sequence = UINT64_MAX;
    TEST_ASSERT_FALSE (
      msg_.get_request_reply_metadata (&kind, &sequence));
    TEST_ASSERT_EQUAL_UINT8 (0, kind);
    TEST_ASSERT_EQUAL_UINT64 (0, sequence);
}

void assert_request_reply_metadata (const zlink::msg_t &msg_,
                                    unsigned char kind_,
                                    uint64_t sequence_)
{
    unsigned char kind = 0;
    uint64_t sequence = 0;
    TEST_ASSERT_TRUE (msg_.get_request_reply_metadata (&kind, &sequence));
    TEST_ASSERT_EQUAL_UINT8 (kind_, kind);
    TEST_ASSERT_EQUAL_UINT64 (sequence_, sequence);
}

void test_all_initializers_start_without_auxiliary_metadata ()
{
    zlink::msg_t vsm;
    TEST_ASSERT_EQUAL_INT (0, vsm.init_size (8));
    assert_no_request_reply_metadata (vsm);
    TEST_ASSERT_EQUAL_STRING ("", vsm.group ());

    zlink::msg_t lmsg;
    TEST_ASSERT_EQUAL_INT (0, lmsg.init_size (64));
    assert_no_request_reply_metadata (lmsg);
    TEST_ASSERT_EQUAL_STRING ("", lmsg.group ());

    unsigned char constant_data[32] = {0};
    zlink::msg_t cmsg;
    TEST_ASSERT_EQUAL_INT (
      0, cmsg.init_data (constant_data, sizeof (constant_data), NULL, NULL));
    assert_no_request_reply_metadata (cmsg);
    TEST_ASSERT_EQUAL_STRING ("", cmsg.group ());

    unsigned char external_data[32] = {0};
    zlink::msg_t::content_t external_content;
    zlink::msg_t zclmsg;
    TEST_ASSERT_EQUAL_INT (
      0, zclmsg.init_external_storage (&external_content, external_data,
                                       sizeof (external_data),
                                       ignore_external_free, NULL));
    assert_no_request_reply_metadata (zclmsg);
    TEST_ASSERT_EQUAL_STRING ("", zclmsg.group ());

    TEST_ASSERT_EQUAL_INT (0, zclmsg.close ());
    TEST_ASSERT_EQUAL_INT (0, cmsg.close ());
    TEST_ASSERT_EQUAL_INT (0, lmsg.close ());
    TEST_ASSERT_EQUAL_INT (0, vsm.close ());
}

void exercise_request_reply_copy_and_move (zlink::msg_t &source_)
{
    TEST_ASSERT_EQUAL_INT (
      0, source_.set_request_reply_metadata (request_kind, request_sequence));

    zlink::msg_t copied;
    TEST_ASSERT_EQUAL_INT (0, copied.init ());
    TEST_ASSERT_EQUAL_INT (0, copied.copy (source_));
    assert_request_reply_metadata (source_, request_kind, request_sequence);
    assert_request_reply_metadata (copied, request_kind, request_sequence);
    TEST_ASSERT_EQUAL_INT (0, copied.close ());

    zlink::msg_t moved;
    TEST_ASSERT_EQUAL_INT (0, moved.init ());
    TEST_ASSERT_EQUAL_INT (0, moved.move (source_));
    assert_request_reply_metadata (moved, request_kind, request_sequence);
    assert_no_request_reply_metadata (source_);
    TEST_ASSERT_EQUAL_INT (0, moved.close ());
    TEST_ASSERT_EQUAL_INT (0, source_.close ());
}

int external_free_count = 0;

void count_external_free (void *, void *)
{
    ++external_free_count;
}

void test_copy_and_move_preserve_metadata_for_every_payload_type ()
{
    zlink::msg_t vsm;
    TEST_ASSERT_EQUAL_INT (0, vsm.init_size (8));
    exercise_request_reply_copy_and_move (vsm);

    zlink::msg_t lmsg;
    TEST_ASSERT_EQUAL_INT (0, lmsg.init_size (64));
    exercise_request_reply_copy_and_move (lmsg);

    unsigned char constant_data[32] = {0};
    zlink::msg_t cmsg;
    TEST_ASSERT_EQUAL_INT (
      0, cmsg.init_data (constant_data, sizeof (constant_data), NULL, NULL));
    exercise_request_reply_copy_and_move (cmsg);

    unsigned char external_data[32] = {0};
    zlink::msg_t::content_t external_content;
    zlink::msg_t zclmsg;
    external_free_count = 0;
    TEST_ASSERT_EQUAL_INT (
      0, zclmsg.init_external_storage (
           &external_content, external_data, sizeof (external_data),
           count_external_free, NULL));
    exercise_request_reply_copy_and_move (zclmsg);
    TEST_ASSERT_EQUAL_INT (1, external_free_count);
}

void test_view_starts_without_source_metadata ()
{
    zlink::msg_t source;
    zlink::msg_t view;
    TEST_ASSERT_EQUAL_INT (0, source.init_size (64));
    TEST_ASSERT_EQUAL_INT (0, view.init ());
    TEST_ASSERT_EQUAL_INT (
      0, source.set_request_reply_metadata (request_kind, request_sequence));

    TEST_ASSERT_EQUAL_INT (0, view.init_view (source, 4, 32));
    assert_request_reply_metadata (source, request_kind, request_sequence);
    assert_no_request_reply_metadata (view);

    TEST_ASSERT_EQUAL_INT (0, view.close ());
    TEST_ASSERT_EQUAL_INT (0, source.close ());
}

void test_group_and_request_reply_metadata_are_mutually_exclusive ()
{
    zlink::msg_t grouped;
    TEST_ASSERT_EQUAL_INT (0, grouped.init ());
    TEST_ASSERT_EQUAL_INT (0, grouped.set_group ("short-group"));

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, grouped.set_request_reply_metadata (request_kind, request_sequence));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_STRING ("short-group", grouped.group ());

    const char long_group[] = "a-group-name-longer-than-fourteen-bytes";
    TEST_ASSERT_EQUAL_INT (0, grouped.set_group (long_group));
    TEST_ASSERT_TRUE (grouped.has_long_group ());
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, grouped.set_request_reply_metadata (request_kind, request_sequence));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_STRING (long_group, grouped.group ());

    TEST_ASSERT_EQUAL_INT (0, grouped.set_group ("replacement"));
    TEST_ASSERT_FALSE (grouped.has_long_group ());
    TEST_ASSERT_EQUAL_STRING ("replacement", grouped.group ());
    TEST_ASSERT_EQUAL_INT (0, grouped.close ());

    zlink::msg_t request;
    TEST_ASSERT_EQUAL_INT (0, request.init ());
    TEST_ASSERT_EQUAL_INT (
      0, request.set_request_reply_metadata (request_kind, request_sequence));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, request.set_group ("group"));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    assert_request_reply_metadata (request, request_kind, request_sequence);

    request.reset_request_reply_metadata ();
    assert_no_request_reply_metadata (request);
    TEST_ASSERT_EQUAL_INT (0, request.set_group ("group"));
    TEST_ASSERT_EQUAL_STRING ("group", request.group ());
    TEST_ASSERT_EQUAL_INT (0, request.close ());
}

void test_invalid_request_reply_metadata_does_not_change_message ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, msg.set_request_reply_metadata (0, request_sequence));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    assert_no_request_reply_metadata (msg);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1,
                           msg.set_request_reply_metadata (request_kind, 0));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    assert_no_request_reply_metadata (msg);
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void test_public_adopt_transfers_auxiliary_ownership ()
{
    zlink_msg_t source;
    zlink_msg_t destination;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&source));
    zlink::msg_t *source_internal =
      reinterpret_cast<zlink::msg_t *> (&source);
    const char long_group[] = "public-adopt-long-group-name";
    TEST_ASSERT_EQUAL_INT (0, source_internal->set_group (long_group));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_msg_adopt (&destination, &source));
    zlink::msg_t *destination_internal =
      reinterpret_cast<zlink::msg_t *> (&destination);
    TEST_ASSERT_EQUAL_STRING (long_group, destination_internal->group ());
    TEST_ASSERT_EQUAL_STRING ("", source_internal->group ());
    assert_no_request_reply_metadata (*source_internal);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&source));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&destination));

    zlink_msg_t request_source;
    zlink_msg_t request_destination;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init (&request_source));
    zlink::msg_t *request_source_internal =
      reinterpret_cast<zlink::msg_t *> (&request_source);
    TEST_ASSERT_EQUAL_INT (
      0, request_source_internal->set_request_reply_metadata (
           reply_kind, request_sequence));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_msg_adopt (&request_destination, &request_source));
    assert_request_reply_metadata (
      *reinterpret_cast<zlink::msg_t *> (&request_destination), reply_kind,
      request_sequence);
    assert_no_request_reply_metadata (*request_source_internal);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_close (&request_source));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_close (&request_destination));
}

void test_long_group_copy_and_move_keep_one_live_owner ()
{
    const char long_group[] = "copy-move-long-group-name";
    zlink::msg_t source;
    zlink::msg_t copied;
    TEST_ASSERT_EQUAL_INT (0, source.init_size (64));
    TEST_ASSERT_EQUAL_INT (0, copied.init ());
    TEST_ASSERT_EQUAL_INT (0, source.set_group (long_group));
    TEST_ASSERT_EQUAL_INT (0, copied.copy (source));

    TEST_ASSERT_EQUAL_INT (0, source.set_group ("short"));
    TEST_ASSERT_EQUAL_STRING ("short", source.group ());
    TEST_ASSERT_EQUAL_STRING (long_group, copied.group ());
    TEST_ASSERT_EQUAL_INT (0, source.close ());
    TEST_ASSERT_EQUAL_STRING (long_group, copied.group ());

    zlink::msg_t moved;
    TEST_ASSERT_EQUAL_INT (0, moved.init ());
    TEST_ASSERT_EQUAL_INT (0, moved.move (copied));
    TEST_ASSERT_EQUAL_STRING (long_group, moved.group ());
    TEST_ASSERT_EQUAL_STRING ("", copied.group ());
    TEST_ASSERT_EQUAL_INT (0, copied.close ());
    TEST_ASSERT_EQUAL_INT (0, moved.close ());
}

void exercise_bitwise_fan_out_with_one_failed_write (zlink::msg_t &source_)
{
    const char long_group[] = "fan-out-long-group-name";
    TEST_ASSERT_EQUAL_INT (0, source_.set_group (long_group));

    //  Three conceptual writes share the source references. Two queue copies
    //  survive and one write fails before the source is detached.
    source_.add_refs (2);
    zlink::msg_t survivor_a;
    zlink::msg_t survivor_b;
    std::memcpy (static_cast<void *> (&survivor_a),
                 static_cast<const void *> (&source_), sizeof (source_));
    std::memcpy (static_cast<void *> (&survivor_b),
                 static_cast<const void *> (&source_), sizeof (source_));
    TEST_ASSERT_TRUE (source_.rm_refs (1));
    TEST_ASSERT_EQUAL_INT (0, source_.init ());

    TEST_ASSERT_EQUAL_STRING (long_group, survivor_a.group ());
    TEST_ASSERT_EQUAL_STRING (long_group, survivor_b.group ());
    TEST_ASSERT_EQUAL_INT (0, survivor_a.close ());
    TEST_ASSERT_EQUAL_STRING (long_group, survivor_b.group ());
    TEST_ASSERT_EQUAL_INT (0, survivor_b.close ());
    TEST_ASSERT_EQUAL_INT (0, source_.close ());
}

void test_add_refs_rm_refs_simulate_pipe_fan_out_for_all_payload_types ()
{
    zlink::msg_t vsm;
    TEST_ASSERT_EQUAL_INT (0, vsm.init_size (8));
    exercise_bitwise_fan_out_with_one_failed_write (vsm);

    zlink::msg_t lmsg;
    TEST_ASSERT_EQUAL_INT (0, lmsg.init_size (64));
    exercise_bitwise_fan_out_with_one_failed_write (lmsg);

    unsigned char constant_data[32] = {0};
    zlink::msg_t cmsg;
    TEST_ASSERT_EQUAL_INT (
      0, cmsg.init_data (constant_data, sizeof (constant_data), NULL, NULL));
    exercise_bitwise_fan_out_with_one_failed_write (cmsg);

    unsigned char external_data[32] = {0};
    zlink::msg_t::content_t external_content;
    zlink::msg_t zclmsg;
    external_free_count = 0;
    TEST_ASSERT_EQUAL_INT (
      0, zclmsg.init_external_storage (
           &external_content, external_data, sizeof (external_data),
           count_external_free, NULL));
    exercise_bitwise_fan_out_with_one_failed_write (zclmsg);
    TEST_ASSERT_EQUAL_INT (1, external_free_count);
}

void test_rm_refs_releases_all_failed_fan_out_references ()
{
    unsigned char external_data[32] = {0};
    zlink::msg_t::content_t external_content;
    zlink::msg_t msg;
    external_free_count = 0;
    TEST_ASSERT_EQUAL_INT (
      0, msg.init_external_storage (
           &external_content, external_data, sizeof (external_data),
           count_external_free, NULL));
    TEST_ASSERT_EQUAL_INT (0, msg.set_group ("all-failed-fan-out-group"));

    //  Three conceptual pipe writes all fail. rm_refs() owns final release;
    //  distributor then resets the detached source without closing it again.
    msg.add_refs (2);
    TEST_ASSERT_FALSE (msg.rm_refs (3));
    TEST_ASSERT_EQUAL_INT (1, external_free_count);
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void exercise_payload_alias_survives_group_all_failed_fan_out (
  zlink::msg_t &grouped_alias_)
{
    zlink::msg_t payload_alias;
    TEST_ASSERT_EQUAL_INT (0, payload_alias.init ());
    TEST_ASSERT_EQUAL_INT (0, payload_alias.copy (grouped_alias_));

    const char long_group[] =
      "payload-and-group-have-different-alias-counts";
    TEST_ASSERT_EQUAL_INT (0, grouped_alias_.set_group (long_group));
    TEST_ASSERT_TRUE (grouped_alias_.has_long_group ());
    TEST_ASSERT_EQUAL_STRING ("", payload_alias.group ());

    // The payload has two aliases before fan-out, while the long group belongs
    // only to grouped_alias_. Two additional bitwise fan-out references make
    // the payload count four and the group count three. Removing all three
    // grouped references frees the group but deliberately leaves the payload
    // alias alive, so rm_refs() must report the asymmetric final release
    // without freeing or later double-freeing the shared payload.
    grouped_alias_.add_refs (2);
    TEST_ASSERT_FALSE (grouped_alias_.rm_refs (3));
    TEST_ASSERT_EQUAL_INT (0, external_free_count);
    TEST_ASSERT_EQUAL_INT (0, grouped_alias_.init ());
    TEST_ASSERT_EQUAL_STRING ("", grouped_alias_.group ());

    TEST_ASSERT_EQUAL_UINT64 (32, payload_alias.size ());
    TEST_ASSERT_EQUAL_STRING ("", payload_alias.group ());
    TEST_ASSERT_EQUAL_INT (0, payload_alias.close ());
    TEST_ASSERT_EQUAL_INT (1, external_free_count);
    TEST_ASSERT_EQUAL_INT (0, grouped_alias_.close ());
    TEST_ASSERT_EQUAL_INT (1, external_free_count);
}

void test_lmsg_payload_alias_survives_group_all_failed_fan_out ()
{
    unsigned char external_data[32] = {0};
    zlink::msg_t lmsg;
    external_free_count = 0;
    TEST_ASSERT_EQUAL_INT (
      0, lmsg.init_data (external_data, sizeof (external_data),
                         count_external_free, NULL));
    TEST_ASSERT_TRUE (lmsg.is_lmsg ());
    exercise_payload_alias_survives_group_all_failed_fan_out (lmsg);
}

void test_zclmsg_payload_alias_survives_group_all_failed_fan_out ()
{
    unsigned char external_data[32] = {0};
    zlink::msg_t::content_t external_content;
    zlink::msg_t zclmsg;
    external_free_count = 0;
    TEST_ASSERT_EQUAL_INT (
      0, zclmsg.init_external_storage (
           &external_content, external_data, sizeof (external_data),
           count_external_free, NULL));
    TEST_ASSERT_TRUE (zclmsg.is_zcmsg ());
    exercise_payload_alias_survives_group_all_failed_fan_out (zclmsg);
}

void test_rm_refs_releases_and_reuses_pooled_view_content ()
{
    zlink::msg_t source;
    zlink::msg_t view;
    TEST_ASSERT_EQUAL_INT (0, source.init_size (64));
    TEST_ASSERT_EQUAL_INT (0, view.init ());
    TEST_ASSERT_EQUAL_INT (0, view.init_view (source, 4, 32));
    TEST_ASSERT_TRUE (view.is_zcmsg ());
    TEST_ASSERT_EQUAL_INT (0, view.set_group ("pooled-view-fan-out-group"));

    view.add_refs (1);
    TEST_ASSERT_FALSE (view.rm_refs (2));
    TEST_ASSERT_EQUAL_INT (0, view.init ());

    //  Reuse the slice-content pool after the final rm_refs() release. This
    //  would placement-construct over a live mutex-backed counter if the
    //  previous counter had not been destroyed.
    TEST_ASSERT_EQUAL_INT (0, view.init_view (source, 8, 16));
    TEST_ASSERT_TRUE (view.is_zcmsg ());
    TEST_ASSERT_EQUAL_INT (0, view.close ());
    TEST_ASSERT_EQUAL_INT (0, source.close ());
}

void test_raw_relocation_preserves_auxiliary_without_extra_close ()
{
    zlink::msg_t inline_parts[2];
    TEST_ASSERT_EQUAL_INT (0, inline_parts[0].init_size (8));
    TEST_ASSERT_EQUAL_INT (0, inline_parts[1].init_size (64));
    TEST_ASSERT_EQUAL_INT (
      0, inline_parts[0].set_request_reply_metadata (reply_kind,
                                                     request_sequence));
    const char long_group[] = "relocated-long-group-name";
    TEST_ASSERT_EQUAL_INT (0, inline_parts[1].set_group (long_group));

    const size_t expanded_count = 8;
    void *storage = std::malloc (expanded_count * sizeof (zlink::msg_t));
    TEST_ASSERT_NOT_NULL (storage);
    zlink::msg_t *relocated = static_cast<zlink::msg_t *> (storage);
    for (size_t i = 0; i < expanded_count; ++i)
        new (&relocated[i]) zlink::msg_t;
    std::memcpy (static_cast<void *> (relocated),
                 static_cast<const void *> (inline_parts),
                 sizeof (inline_parts));

    //  Relocation transfers ownership; the old slots are reset, not closed.
    TEST_ASSERT_EQUAL_INT (0, inline_parts[0].init ());
    TEST_ASSERT_EQUAL_INT (0, inline_parts[1].init ());
    assert_request_reply_metadata (relocated[0], reply_kind,
                                   request_sequence);
    TEST_ASSERT_EQUAL_STRING (long_group, relocated[1].group ());

    TEST_ASSERT_EQUAL_INT (0, relocated[0].close ());
    TEST_ASSERT_EQUAL_INT (0, relocated[1].close ());
    std::free (storage);
    TEST_ASSERT_EQUAL_INT (0, inline_parts[1].close ());
    TEST_ASSERT_EQUAL_INT (0, inline_parts[0].close ());
}
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_all_initializers_start_without_auxiliary_metadata);
    RUN_TEST (test_copy_and_move_preserve_metadata_for_every_payload_type);
    RUN_TEST (test_view_starts_without_source_metadata);
    RUN_TEST (test_group_and_request_reply_metadata_are_mutually_exclusive);
    RUN_TEST (test_invalid_request_reply_metadata_does_not_change_message);
    RUN_TEST (test_public_adopt_transfers_auxiliary_ownership);
    RUN_TEST (test_long_group_copy_and_move_keep_one_live_owner);
    RUN_TEST (
      test_add_refs_rm_refs_simulate_pipe_fan_out_for_all_payload_types);
    RUN_TEST (test_rm_refs_releases_all_failed_fan_out_references);
    RUN_TEST (
      test_lmsg_payload_alias_survives_group_all_failed_fan_out);
    RUN_TEST (
      test_zclmsg_payload_alias_survives_group_all_failed_fan_out);
    RUN_TEST (test_rm_refs_releases_and_reuses_pooled_view_content);
    RUN_TEST (test_raw_relocation_preserves_auxiliary_without_extra_close);
    return UNITY_END ();
}
