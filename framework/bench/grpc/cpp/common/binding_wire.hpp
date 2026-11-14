/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#ifndef ZLINK_CPP_BENCH_WITH_GRPC_BINDING_WIRE_HPP
#define ZLINK_CPP_BENCH_WITH_GRPC_BINDING_WIRE_HPP

#include "bench.pb.h"

#include <zlink/Contracts/Messaging/message.hpp>

#include <cstddef>
#include <stdexcept>

namespace zlink_cpp_bench
{

inline zlink::message_t
encode_bench_payload_message (const zlink::framework::bench::withgrpc::BenchPayload &payload)
{
    const size_t size = payload.ByteSizeLong ();
    zlink::message_t message = zlink::message_t::allocate (size);
    if (!payload.SerializeToArray (message.data (), static_cast<int> (size)))
        throw std::runtime_error ("cannot serialize BenchPayload");
    return message;
}

} // namespace zlink_cpp_bench

#endif // ZLINK_CPP_BENCH_WITH_GRPC_BINDING_WIRE_HPP
