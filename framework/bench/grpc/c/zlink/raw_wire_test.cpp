// SPDX-License-Identifier: MPL-2.0
// Include each driver's private encoder so the frozen dump checks production code.
#define main bench_driver_main
#ifdef RAW_WIRE_SERVER_TEST
#include "bench_zlink_server.cpp"
#else
#include "bench_zlink_client.cpp"
#endif
#undef main

#include "bench.pb.h"

#include <stdexcept>

namespace
{
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

void check_wire (size_t size)
{
    const unsigned char golden[] = {
      0x0a, 0x1d, 0x4b, 0x4e, 0x4c, 0x5a, 0x04, 0x03, 0x02, 0x01, 0x01,
      0x1d, 0x00, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
      0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11};
    std::string body (size, static_cast<char> (0xab));
    require (zlink_c_bench::stamp_payload (&body[0], size, 0x01020304,
              zlink_c_bench::phase_active, 0x0102030405060708ULL), "stamp failed");
    zlink_c_bench::write_u64_le (reinterpret_cast<unsigned char *> (&body[21]),
                               0x1112131415161718ULL);
    zlink::framework::bench::withgrpc::BenchPayload typed;
    typed.set_body (body);
    const std::string protobuf = typed.SerializeAsString ();
    if (size == 29)
        require (protobuf == std::string (reinterpret_cast<const char *> (golden),
                                          sizeof golden), "protobuf differs from frozen dump");

    zlink_msg_t encoded;
#ifdef RAW_WIRE_SERVER_TEST
    zlink_msg_t request;
    require (zlink_msg_init_size (&request, protobuf.size ()) == ZLINK_CONFIG_OK,
             "request allocation failed");
    std::memcpy (zlink_msg_data (&request), protobuf.data (), protobuf.size ());
    require (make_response_body (&request, &encoded), "raw response failed");
    zlink_msg_close (&request);
#else
    require (make_payload_body_msg (size, 0x01020304, zlink_c_bench::phase_active,
               0x0102030405060708ULL, &encoded), "raw request failed");
    // The production stamp uses a monotonic clock. Freeze only its timestamp.
    auto *encoded_bytes = static_cast<unsigned char *> (zlink_msg_data (&encoded));
    zlink_c_bench::write_u64_le (encoded_bytes + zlink_msg_size (&encoded) - size + 21,
                               0x1112131415161718ULL);
#endif
    require (std::string (static_cast<const char *> (zlink_msg_data (&encoded)),
                         zlink_msg_size (&encoded)) == protobuf,
             "raw wire differs from protobuf");
    zlink::framework::bench::withgrpc::BenchPayload decoded;
    require (decoded.ParseFromArray (zlink_msg_data (&encoded),
               static_cast<int> (zlink_msg_size (&encoded))) && decoded.body () == body,
             "protobuf body round trip failed");
    zlink_msg_close (&encoded);
}
}

int main ()
{
    for (const size_t size : {29, 127, 128, 1024, 4096})
        check_wire (size);
    std::puts ("raw wire: 29-byte frozen dump and 127/128/1024/4096-byte protobuf parity passed");
}
