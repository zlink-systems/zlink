/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}
void verify_reply_header_cost ()
{
    namespace wire = zlink::framework::runtime::protocol;
    using namespace zlink::framework;
    std::vector<std::uint8_t> baseline;
    const auto budget =
      tests::measure_allocations ([&] { baseline.reserve (wire::reply_header_t::encoded_size); });
    std::vector<std::uint8_t> encoded;
    const auto actual =
      tests::measure_allocations ([&] { encoded = wire::encode_reply_header (7, 0, 0); });
    const auto decoded = wire::decode_reply_header (encoded);
    require (encoded.size () == wire::reply_header_t::encoded_size && decoded.correlation == 7,
             "reply header wire representation changed");
    std::cout << "reply-header count=" << actual.count << " bytes=" << actual.bytes
              << " budget-count=" << budget.count << " budget-bytes=" << budget.bytes << '\n';
    require (actual.count <= budget.count && actual.bytes <= budget.bytes,
             "reply header allocates more than its final wire buffer");
    const auto injected = tests::measure_allocations ([&] {
        encoded = wire::encode_reply_header (7, 0, 0);
        void *(*volatile allocate) (std::size_t) = &::operator new;
        auto *memory = allocate (wire::reply_header_t::encoded_size);
        ::operator delete (memory);
    });
    require (injected.count > budget.count && injected.bytes > budget.bytes,
             "reply header cost check did not reject an added allocation");
}

void verify_payload_accounting_cost ()
{
    namespace wire = zlink::framework::runtime::protocol;
    using namespace zlink::framework;
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from (std::string (1024, 'x')));
    const auto payload = wire::encode_application_payload (
      wire::application_payload_t::from_parts (std::move (parts)));
    std::size_t bytes = 0;
    const auto actual =
      tests::measure_allocations ([&] { bytes = wire::application_payload_hwm_bytes (payload); });
    std::cout << "payload-accounting count=" << actual.count << " bytes=" << actual.bytes
              << " application-bytes=" << bytes << '\n';
    require (bytes == 1024, "payload HWM accounting changed");
    require (actual.count == 0 && actual.bytes == 0,
             "payload HWM accounting copies header strings");
}

}
int main (int argc, char **argv)
{
    try {
        if (argc == 2 && std::string_view (argv[1]) == "--payload-accounting-only") {
            verify_payload_accounting_cost ();
            return 0;
        }
        verify_reply_header_cost ();
        verify_payload_accounting_cost ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
