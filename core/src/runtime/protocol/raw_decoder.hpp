/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_RAW_DECODER_HPP_INCLUDED__
#define __ZLINK_RAW_DECODER_HPP_INCLUDED__

#include "protocol/i_decoder.hpp"
#include "protocol/decoder_allocators.hpp"
#include "core/msg.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
//  Decoder for raw STREAM payload (no extra framing).
class raw_decoder_t ZLINK_FINAL : public i_decoder
{
  public:
    raw_decoder_t (size_t bufsize_, int64_t maxmsgsize_);
    raw_decoder_t (size_t bufsize_, int64_t maxmsgsize_, size_t max_buffer_size_);
    ~raw_decoder_t ();

    void get_buffer (unsigned char **data_, size_t *size_) ZLINK_FINAL;
    int decode (const unsigned char *data_, size_t size_, size_t &bytes_used_) ZLINK_FINAL;
    void resize_buffer (size_t new_size_) ZLINK_FINAL
    {
        // STREAM/raw growth path: keep capacity monotonic to avoid
        // shrink/re-grow churn in steady state.
        if (new_size_ > _allocator.allocation_size ())
            _allocator.set_allocation_size (new_size_);
    }

    msg_t *msg () { return &_in_progress; }

  private:
    msg_t _in_progress;
    shared_message_memory_allocator _allocator;
    const int64_t _max_msg_size;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (raw_decoder_t)
};
}

#endif
