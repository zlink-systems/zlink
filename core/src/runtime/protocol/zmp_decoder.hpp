/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ZMP_DECODER_HPP_INCLUDED__
#define __ZLINK_ZMP_DECODER_HPP_INCLUDED__

#include "protocol/decoder.hpp"
#include "protocol/decoder_allocators.hpp"
#include "protocol/zmp_protocol.hpp"

namespace zlink
{
//  Decoder for ZMP framing protocol (v1).
class zmp_decoder_t ZLINK_FINAL
    : public decoder_base_t<zmp_decoder_t, shared_message_memory_allocator>
{
  public:
    typedef int (*frame_admission_handler_t) (void *subject_,
                                              uint32_t payload_bytes_,
                                              unsigned char msg_flags_,
                                              void **reservation_out_);
    typedef void (*frame_reservation_release_handler_t) (void *subject_,
                                                         void *reservation_);

    zmp_decoder_t (size_t bufsize_, int64_t maxmsgsize_);
    ~zmp_decoder_t ();

    msg_t *msg () { return &_in_progress; }
    uint8_t error_code () const { return _error_code; }
    void set_frame_admission_handler (frame_admission_handler_t handler_,
                                      frame_reservation_release_handler_t release_handler_,
                                      void *subject_);
    bool allocation_backpressured () const;
    int retry_frame_admission ();
    void **frame_reservation_slot ();
    void discard_frame_reservation ();
    void detach_frame_admission () ZLINK_OVERRIDE;
    int stream_end () ZLINK_OVERRIDE;

  private:
    int header_ready (unsigned char const *read_from_);
    int sequence_ready (unsigned char const *read_from_);
    int body_ready (unsigned char const *read_from_);

    int size_ready (uint32_t size_, unsigned char const *read_from_);
    int validate_header_and_admit (unsigned char const *read_from_);
    void complete_frame ();
    int fail_protocol (uint8_t error_code_);
    void release_frame_reservation ();

    enum frame_stage_t
    {
        reading_base_header,
        reading_sequence_extension,
        waiting_frame_admission,
        reading_payload
    };

    unsigned char _tmpbuf[zmp_request_reply_header_size];
    unsigned char _msg_flags;
    unsigned char _wire_flags;
    unsigned char _wire_kind;
    uint64_t _request_sequence;
    uint8_t _error_code;
    msg_t _in_progress;
    const uint32_t _max_msg_size_effective;
    const uint64_t _max_application_message_size;
    frame_admission_handler_t _frame_admission_handler;
    frame_reservation_release_handler_t _frame_reservation_release_handler;
    void *_frame_admission_subject;
    void *_frame_reservation;
    uint32_t _pending_msg_size;
    const unsigned char *_pending_read_from;
    bool _allocation_backpressured;
    bool _application_multipart_in_progress;
    bool _next_application_multipart_in_progress;
    uint64_t _application_multipart_payload_size;
    uint64_t _next_application_multipart_payload_size;
    frame_stage_t _frame_stage;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (zmp_decoder_t)
};
}

#endif
