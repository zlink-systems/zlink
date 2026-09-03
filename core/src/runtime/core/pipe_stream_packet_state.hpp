/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PIPE_STREAM_PACKET_STATE_HPP_INCLUDED__
#define __ZLINK_PIPE_STREAM_PACKET_STATE_HPP_INCLUDED__

#include <stddef.h>

#include "core/msg.hpp"

namespace zlink
{
struct pipe_stream_packet_state_t
{
    enum stage_t
    {
        prefix_stage,
        header_stage,
        body_stage
    };

    enum storage_t : unsigned char
    {
        separate_storage,
        coalesced_storage
    };

    pipe_stream_packet_state_t ();
    ~pipe_stream_packet_state_t ();

    void reset ();

    stage_t stage;
    storage_t storage;
    unsigned char prefix[6];
    size_t prefix_used;
    size_t header_size;
    size_t body_size;
    size_t header_used;
    size_t body_used;
    msg_t header;
    msg_t body;
};

#ifdef ZLINK_BUILD_TESTS
enum stream_packet_allocation_failpoint_t
{
    stream_packet_allocation_none = 0,
    stream_packet_allocation_backing,
    stream_packet_allocation_body_view
};

bool test_consume_stream_packet_allocation_failpoint (
  stream_packet_allocation_failpoint_t failpoint_);
#endif
}

#endif
