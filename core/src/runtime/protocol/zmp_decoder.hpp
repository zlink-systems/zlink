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

  private:
    int header_ready (unsigned char const *read_from_);
    int body_ready (unsigned char const *read_from_);

    int size_ready (uint32_t size_, unsigned char const *read_from_);
    void release_frame_reservation ();

    unsigned char _tmpbuf[zmp_header_size];
    unsigned char _msg_flags;
    uint8_t _error_code;
    msg_t _in_progress;
    const uint32_t _max_msg_size_effective;
    frame_admission_handler_t _frame_admission_handler;
    frame_reservation_release_handler_t _frame_reservation_release_handler;
    void *_frame_admission_subject;
    void *_frame_reservation;
    uint32_t _pending_msg_size;
    const unsigned char *_pending_read_from;
    bool _allocation_backpressured;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (zmp_decoder_t)
};
}

#endif
