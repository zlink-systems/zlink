// SPDX-License-Identifier: MPL-2.0
#ifndef ZLINK_C_BENCH_RAW_WIRE_HPP
#define ZLINK_C_BENCH_RAW_WIRE_HPP

#include "bench.pb.h"
#include <zlink.h>

inline bool serialize_bench_payload (
  const zlink::framework::bench::withgrpc::BenchPayload &payload, zlink_msg_t *msg)
{
    const size_t size = payload.ByteSizeLong ();
    if (zlink_msg_init_size (msg, size) != ZLINK_CONFIG_OK)
        return false;
    if (payload.SerializeToArray (zlink_msg_data (msg), static_cast<int> (size)))
        return true;
    zlink_msg_close (msg);
    return false;
}

#endif
