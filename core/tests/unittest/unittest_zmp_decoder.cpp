/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "utils/ip.hpp"
#include "core/msg.hpp"
#include "protocol/decoder_allocators.hpp"
#include "protocol/wire.hpp"
#include "protocol/zmp_decoder.hpp"
#include "protocol/zmp_metadata.hpp"
#include "protocol/zmp_protocol.hpp"

#include <atomic>
#include <limits>
#include <thread>
#include <unity.h>
#include <vector>

void setUp ()
{
}

void tearDown ()
{
}

static void build_header (unsigned char *buf_, unsigned char flags_, uint32_t body_len_)
{
    buf_[0] = zlink::zmp_magic;
    buf_[1] = zlink::zmp_version;
    buf_[2] = flags_;
    buf_[3] = 0;
    zlink::put_uint32 (buf_ + 4, body_len_);
}

static void init_allocator_backed_message (
  zlink::shared_message_memory_allocator &allocator_, unsigned char *data_,
  zlink::msg_t *msg_)
{
    const int rc = msg_->init (
      data_, zlink::msg_t::max_vsm_size,
      &zlink::shared_message_memory_allocator::call_dec_ref,
      allocator_.buffer (), allocator_.provide_content ());
    TEST_ASSERT_EQUAL_INT (0, rc);
    TEST_ASSERT_TRUE (msg_->is_zcmsg ());
    allocator_.advance_content ();
    allocator_.inc_ref ();
}

struct fake_frame_admission_t
{
    fake_frame_admission_t () :
        allow (false), calls (0), releases (0), payload_bytes (0), flags (0)
    {
    }

    static int reserve (void *subject_, uint32_t payload_bytes_,
                        unsigned char flags_, void **reservation_out_)
    {
        fake_frame_admission_t *self =
          static_cast<fake_frame_admission_t *> (subject_);
        ++self->calls;
        self->payload_bytes = payload_bytes_;
        self->flags = flags_;
        *reservation_out_ = NULL;
        if (!self->allow) {
            errno = EAGAIN;
            return -1;
        }
        *reservation_out_ = self;
        return 0;
    }

    static void release (void *subject_, void *reservation_)
    {
        fake_frame_admission_t *self =
          static_cast<fake_frame_admission_t *> (subject_);
        TEST_ASSERT_EQUAL_PTR (self, reservation_);
        ++self->releases;
    }

    bool allow;
    int calls;
    int releases;
    uint32_t payload_bytes;
    unsigned char flags;
};

void test_invalid_magic ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, 0, 0);
    buf[0] = 0x00;
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_invalid_magic, decoder.error_code ());
}

void test_version_mismatch ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, 0, 0);
    buf[1] = static_cast<unsigned char> (zlink::zmp_version + 1);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_version_mismatch, decoder.error_code ());
}

void test_flags_invalid ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, zlink::zmp_flag_control | zlink::zmp_flag_more, 0);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_flags_invalid, decoder.error_code ());
}

void test_subscribe_cancel_invalid ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, zlink::zmp_flag_subscribe | zlink::zmp_flag_cancel, 0);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_flags_invalid, decoder.error_code ());
}

void test_body_too_large ()
{
    zlink::zmp_decoder_t decoder (64, 16);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, 0, 32);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_body_too_large, decoder.error_code ());
}

void test_more_identity_allowed ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, zlink::zmp_flag_more | zlink::zmp_flag_identity, 0);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (1, rc);
    const unsigned char flags = decoder.msg ()->flags ();
    TEST_ASSERT_TRUE (flags & zlink::msg_t::more);
    TEST_ASSERT_TRUE (flags & zlink::msg_t::routing_id);
}

void test_payload_admission_precedes_body_allocation_and_can_retry ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    fake_frame_admission_t admission;
    decoder.set_frame_admission_handler (&fake_frame_admission_t::reserve,
                                         NULL, &admission);

    unsigned char frame[zlink::zmp_header_size + 4];
    build_header (frame, zlink::zmp_flag_more, 4);
    memcpy (frame + zlink::zmp_header_size, "body", 4);

    size_t processed = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, decoder.decode (frame, sizeof (frame), processed));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_UINT64 (zlink::zmp_header_size, processed);
    TEST_ASSERT_TRUE (decoder.allocation_backpressured ());
    TEST_ASSERT_EQUAL_UINT32 (4, admission.payload_bytes);
    TEST_ASSERT_TRUE (admission.flags & zlink::msg_t::more);
    TEST_ASSERT_EQUAL_UINT64 (0, decoder.msg ()->size ());

    admission.allow = true;
    TEST_ASSERT_EQUAL_INT (0, decoder.retry_frame_admission ());
    TEST_ASSERT_FALSE (decoder.allocation_backpressured ());
    TEST_ASSERT_EQUAL_INT (2, admission.calls);
    TEST_ASSERT_EQUAL_PTR (&admission, *decoder.frame_reservation_slot ());
    *decoder.frame_reservation_slot () = NULL;

    processed = 0;
    TEST_ASSERT_EQUAL_INT (
      1, decoder.decode (frame + zlink::zmp_header_size, 4, processed));
    TEST_ASSERT_EQUAL_UINT64 (4, processed);
    TEST_ASSERT_EQUAL_UINT64 (4, decoder.msg ()->size ());
    TEST_ASSERT_EQUAL_MEMORY ("body", decoder.msg ()->data (), 4);
}

void test_admission_retry_consumes_only_current_zero_copy_frame ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    fake_frame_admission_t admission;
    decoder.set_frame_admission_handler (&fake_frame_admission_t::reserve,
                                         NULL, &admission);

    unsigned char *input = NULL;
    size_t input_capacity = 0;
    decoder.get_buffer (&input, &input_capacity);
    const size_t frame_size = zlink::zmp_header_size + 4;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64 (2 * frame_size, input_capacity);

    build_header (input, zlink::zmp_flag_more, 4);
    memcpy (input + zlink::zmp_header_size, "body", 4);
    build_header (input + frame_size, 0, 4);
    memcpy (input + frame_size + zlink::zmp_header_size, "next", 4);

    size_t processed = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, decoder.decode (input, 2 * frame_size, processed));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_UINT64 (zlink::zmp_header_size, processed);

    admission.allow = true;
    TEST_ASSERT_EQUAL_INT (0, decoder.retry_frame_admission ());

    processed = 0;
    TEST_ASSERT_EQUAL_INT (
      1, decoder.decode (input + zlink::zmp_header_size,
                         2 * frame_size - zlink::zmp_header_size, processed));
    TEST_ASSERT_EQUAL_UINT64 (4, processed);
    TEST_ASSERT_EQUAL_MEMORY ("body", decoder.msg ()->data (), 4);
    *decoder.frame_reservation_slot () = NULL;

    processed = 0;
    TEST_ASSERT_EQUAL_INT (
      1, decoder.decode (input + frame_size, frame_size, processed));
    TEST_ASSERT_EQUAL_UINT64 (frame_size, processed);
    TEST_ASSERT_EQUAL_MEMORY ("next", decoder.msg ()->data (), 4);
    *decoder.frame_reservation_slot () = NULL;
}

void test_protocol_control_bypasses_application_admission ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    fake_frame_admission_t admission;
    decoder.set_frame_admission_handler (&fake_frame_admission_t::reserve,
                                         NULL, &admission);

    unsigned char frame[zlink::zmp_header_size];
    build_header (frame, zlink::zmp_flag_control, 0);
    size_t processed = 0;
    TEST_ASSERT_EQUAL_INT (1, decoder.decode (frame, sizeof (frame), processed));
    TEST_ASSERT_EQUAL_INT (0, admission.calls);
    TEST_ASSERT_FALSE (decoder.allocation_backpressured ());
}

void test_synchronously_discarded_frame_releases_reservation_once ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    fake_frame_admission_t admission;
    admission.allow = true;
    decoder.set_frame_admission_handler (&fake_frame_admission_t::reserve,
                                         &fake_frame_admission_t::release,
                                         &admission);

    unsigned char frame[zlink::zmp_header_size];
    build_header (frame, zlink::zmp_flag_identity, 0);
    size_t processed = 0;
    TEST_ASSERT_EQUAL_INT (1, decoder.decode (frame, sizeof (frame), processed));
    TEST_ASSERT_EQUAL_PTR (&admission, *decoder.frame_reservation_slot ());

    decoder.discard_frame_reservation ();
    TEST_ASSERT_NULL (*decoder.frame_reservation_slot ());
    TEST_ASSERT_EQUAL_INT (1, admission.releases);
    decoder.discard_frame_reservation ();
    TEST_ASSERT_EQUAL_INT (1, admission.releases);
}

void test_metadata_parse_valid ()
{
    std::vector<unsigned char> buf;
    zlink::zmp_metadata::append_property (buf, "Socket-Type", "PAIR", 4);

    zlink::metadata_t::dict_t out;
    const int rc = zlink::zmp_metadata::parse (&buf[0], buf.size (), out);
    TEST_ASSERT_EQUAL_INT (0, rc);
    TEST_ASSERT_EQUAL_STRING ("PAIR", out["Socket-Type"].c_str ());
}

void test_metadata_parse_invalid ()
{
    std::vector<unsigned char> buf;
    zlink::zmp_metadata::append_property (buf, "Socket-Type", "PAIR", 4);
    if (!buf.empty ())
        buf.pop_back ();

    zlink::metadata_t::dict_t out;
    const int rc = zlink::zmp_metadata::parse (&buf[0], buf.size (), out);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
}

void test_metadata_add_basic_properties ()
{
    zlink::options_t options;
    options.type = ZLINK_CORE_SOCKET_ROUTER;
    const char *routing_id = "RID";
    memcpy (options.routing_id, routing_id, 3);
    options.routing_id_size = 3;

    std::vector<unsigned char> buf;
    zlink::zmp_metadata::add_basic_properties (options, buf);

    zlink::metadata_t::dict_t out;
    const int rc = zlink::zmp_metadata::parse (&buf[0], buf.size (), out);
    TEST_ASSERT_EQUAL_INT (0, rc);
    TEST_ASSERT_EQUAL_STRING ("ROUTER", out["Socket-Type"].c_str ());
    TEST_ASSERT_EQUAL_STRING ("RID", out["Routing-Id"].c_str ());
}

void test_shared_message_allocator_size_checks_overflow ()
{
    std::size_t allocation_size = 0;
    const std::size_t header_size =
      zlink::shared_message_memory_allocator::buffer_header_size_for_test ();
    TEST_ASSERT_TRUE (zlink::shared_message_memory_allocator::allocation_size_for_test (
      64, 2, &allocation_size));
    TEST_ASSERT_EQUAL_UINT64 (
      64 + header_size + 2 * sizeof (zlink::msg_t::content_t),
      allocation_size);

    const std::size_t max_size = std::numeric_limits<std::size_t>::max ();
    TEST_ASSERT_FALSE (zlink::shared_message_memory_allocator::allocation_size_for_test (
      64, max_size / sizeof (zlink::msg_t::content_t) + 1, &allocation_size));

    TEST_ASSERT_FALSE (zlink::shared_message_memory_allocator::allocation_size_for_test (
      max_size - header_size + 1, 0, &allocation_size));

    TEST_ASSERT_FALSE (zlink::shared_message_memory_allocator::allocation_size_for_test (
      max_size - header_size, 1, &allocation_size));
}

void test_shared_message_allocator_reuses_one_lifecycle_owned_spare ()
{
    zlink::shared_message_memory_allocator allocator (128, 4);

    unsigned char *const first_data = allocator.allocate ();
    zlink::msg_t first;
    init_allocator_backed_message (allocator, first_data, &first);

    unsigned char *const second_data = allocator.allocate ();
    zlink::msg_t second;
    init_allocator_backed_message (allocator, second_data, &second);

    unsigned char *const third_data = allocator.allocate ();
    zlink::msg_t third;
    init_allocator_backed_message (allocator, third_data, &third);

    TEST_ASSERT_EQUAL_INT (0, first.close ());
    TEST_ASSERT_EQUAL_UINT64 (1, allocator.cached_buffer_count_for_test ());

    // A second returned block is freed instead of growing the cache.
    TEST_ASSERT_EQUAL_INT (0, second.close ());
    TEST_ASSERT_EQUAL_UINT64 (1, allocator.cached_buffer_count_for_test ());

    // The current block is still message-owned, so the next read takes the
    // one spare that came from the first block.
    TEST_ASSERT_EQUAL_PTR (first_data, allocator.allocate ());
    TEST_ASSERT_EQUAL_UINT64 (0, allocator.cached_buffer_count_for_test ());

    TEST_ASSERT_EQUAL_INT (0, third.close ());
}

void test_shared_message_allocator_rejects_mismatched_spare_capacity ()
{
    zlink::shared_message_memory_allocator allocator (64, 2, 128);

    unsigned char *const small_data = allocator.allocate ();
    zlink::msg_t small;
    init_allocator_backed_message (allocator, small_data, &small);

    allocator.set_allocation_size (128);
    unsigned char *const large_data = allocator.allocate ();
    zlink::msg_t large;
    init_allocator_backed_message (allocator, large_data, &large);

    TEST_ASSERT_EQUAL_INT (0, small.close ());
    TEST_ASSERT_EQUAL_UINT64 (1, allocator.cached_buffer_count_for_test ());

    TEST_ASSERT_NOT_NULL (allocator.allocate ());
    TEST_ASSERT_EQUAL_UINT64 (128, allocator.allocation_size ());
    TEST_ASSERT_EQUAL_UINT64 (0, allocator.cached_buffer_count_for_test ());

    TEST_ASSERT_EQUAL_INT (0, large.close ());
    // Dynamic growth above the allocator's initial read target is never
    // retained per connection.
    TEST_ASSERT_EQUAL_UINT64 (0, allocator.cached_buffer_count_for_test ());
}

void test_shared_message_allocator_cross_thread_recycle_is_synchronized ()
{
    zlink::shared_message_memory_allocator allocator (128, 3);

    unsigned char *const first_data = allocator.allocate ();
    zlink::msg_t first;
    init_allocator_backed_message (allocator, first_data, &first);

    unsigned char *const second_data = allocator.allocate ();
    zlink::msg_t second;
    init_allocator_backed_message (allocator, second_data, &second);

    int close_rc = -1;
    std::thread closer ([&first, &close_rc] () { close_rc = first.close (); });
    TEST_ASSERT_NOT_NULL (allocator.allocate ());
    closer.join ();

    TEST_ASSERT_EQUAL_INT (0, close_rc);
    TEST_ASSERT_EQUAL_INT (0, second.close ());
    TEST_ASSERT_EQUAL_UINT64 (1, allocator.cached_buffer_count_for_test ());
}

void test_shared_message_allocator_state_outlives_decoder_owner ()
{
    zlink::msg_t survivor;
    zlink::shared_message_memory_allocator *allocator =
      new zlink::shared_message_memory_allocator (128, 1);
    unsigned char *const data = allocator->allocate ();
    init_allocator_backed_message (*allocator, data, &survivor);

    delete allocator;

    // The detached buffer owns the recycle state until its final message is
    // closed; this must remain valid after the decoder-side owner is gone.
    TEST_ASSERT_EQUAL_INT (0, survivor.close ());
}

void test_shared_message_allocator_shutdown_races_final_close ()
{
    for (std::size_t i = 0; i != 64; ++i) {
        zlink::msg_t survivor;
        zlink::shared_message_memory_allocator *allocator =
          new zlink::shared_message_memory_allocator (128, 1);
        unsigned char *const data = allocator->allocate ();
        init_allocator_backed_message (*allocator, data, &survivor);
        // Detach the first block so survivor.close() is its final owner and
        // races CLOSED publication rather than only the buffer refcount.
        (void) allocator->allocate ();

        std::atomic<bool> start (false);
        int close_rc = -1;
        std::thread closer ([&survivor, &start, &close_rc] () {
            while (!start.load (std::memory_order_acquire))
                std::this_thread::yield ();
            close_rc = survivor.close ();
        });

        start.store (true, std::memory_order_release);
        if (i % 2 == 0)
            std::this_thread::yield ();
        delete allocator;
        closer.join ();

        TEST_ASSERT_EQUAL_INT (0, close_rc);
    }
}

int main (void)
{
    UNITY_BEGIN ();

    zlink::initialize_network ();
    setup_test_environment ();

    RUN_TEST (test_invalid_magic);
    RUN_TEST (test_version_mismatch);
    RUN_TEST (test_flags_invalid);
    RUN_TEST (test_subscribe_cancel_invalid);
    RUN_TEST (test_body_too_large);
    RUN_TEST (test_more_identity_allowed);
    RUN_TEST (test_payload_admission_precedes_body_allocation_and_can_retry);
    RUN_TEST (test_admission_retry_consumes_only_current_zero_copy_frame);
    RUN_TEST (test_protocol_control_bypasses_application_admission);
    RUN_TEST (test_synchronously_discarded_frame_releases_reservation_once);
    RUN_TEST (test_metadata_parse_valid);
    RUN_TEST (test_metadata_parse_invalid);
    RUN_TEST (test_metadata_add_basic_properties);
    RUN_TEST (test_shared_message_allocator_size_checks_overflow);
    RUN_TEST (test_shared_message_allocator_reuses_one_lifecycle_owned_spare);
    RUN_TEST (test_shared_message_allocator_rejects_mismatched_spare_capacity);
    RUN_TEST (test_shared_message_allocator_cross_thread_recycle_is_synchronized);
    RUN_TEST (test_shared_message_allocator_state_outlives_decoder_owner);
    RUN_TEST (test_shared_message_allocator_shutdown_races_final_close);

    zlink::shutdown_network ();

    return UNITY_END ();
}
