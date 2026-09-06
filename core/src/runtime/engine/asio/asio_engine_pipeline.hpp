/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_ENGINE_PIPELINE_HPP_INCLUDED__
#define __ZLINK_ASIO_ENGINE_PIPELINE_HPP_INCLUDED__

#include "core/msg.hpp"

#include <deque>
#include <vector>

namespace zlink
{
//  One buffered read that arrived while input was stopped. Bytes the decoder
//  already consumed are dropped by advancing offset, never by moving the tail
//  of the vector, so a partially drained chunk costs nothing to keep.
struct asio_rx_chunk_t
{
    std::vector<unsigned char> data;
    size_t offset;

    asio_rx_chunk_t () : offset (0) {}
    size_t size () const { return data.size () > offset ? data.size () - offset : 0; }
};

struct asio_engine_pipeline_t
{
    asio_engine_pipeline_t () :
        read_from_pending_pool (false),
        last_speculative_read_bytes (0),
        total_pending_bytes (0),
        io_error (false),
        read_pending (false),
        write_pending (false),
        handshake_pending (false),
        async_zero_copy (false),
        async_gather (false),
        in_read_drain (false),
        gather_header_size (0),
        gather_body (NULL),
        gather_body_size (0),
        read_buffer_ptr (NULL),
        last_read_request_size (0),
        last_read_had_partial_prefix (false),
        stream_decoder_read_target_size (0),
        stream_decoder_read_target_max (0),
        stream_decoder_read_target_full_hits (0),
        stream_encoder_write_target_size (0),
        stream_encoder_write_target_max (0),
        stream_encoder_write_target_full_hits (0),
        stream_encoder_pending_resize_size (0)
    {
    }

    std::vector<unsigned char> read_buffer;
    std::vector<unsigned char> write_buffer;
    std::deque<asio_rx_chunk_t> pending_rx_chunks;
    std::vector<asio_rx_chunk_t> pending_rx_chunk_pool;
    std::vector<unsigned char> pending_read_buffer;
    bool read_from_pending_pool;
    size_t last_speculative_read_bytes;
    size_t total_pending_bytes;
    msg_t tx_msg;
    bool io_error;
    bool read_pending;
    bool write_pending;
    bool handshake_pending;
    bool async_zero_copy;
    bool async_gather;
    bool in_read_drain;
    unsigned char gather_header[64];
    size_t gather_header_size;
    const unsigned char *gather_body;
    size_t gather_body_size;
    unsigned char *read_buffer_ptr;
    size_t last_read_request_size;
    bool last_read_had_partial_prefix;
    size_t stream_decoder_read_target_size;
    size_t stream_decoder_read_target_max;
    size_t stream_decoder_read_target_full_hits;
    size_t stream_encoder_write_target_size;
    size_t stream_encoder_write_target_max;
    size_t stream_encoder_write_target_full_hits;
    size_t stream_encoder_pending_resize_size;
};
}

#endif
