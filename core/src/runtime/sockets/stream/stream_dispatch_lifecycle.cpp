/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/stream/stream.hpp"

int zlink::stream_t::stream_mark_raw_part_receive ()
{
    if (options.stream_recv_mode != ZLINK_STREAM_RECV_MODE_RAW) {
        errno = ENOTSUP;
        return -1;
    }

    return 0;
}
