/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{

void test_string_roundtrip ()
{
    zlink::message_t msg = zlink::message_t::from ("alpha");
    assert (msg.valid ());
    assert (msg.to_string () == "alpha");
}

void test_bytes_roundtrip ()
{
    std::vector<uint8_t> bytes;
    bytes.push_back (0x10);
    bytes.push_back (0x20);
    bytes.push_back (0x30);

    zlink::message_t msg = zlink::message_t::from (bytes);
    assert (msg.valid ());

    const std::vector<uint8_t> out = msg.to_bytes ();
    assert (out == bytes);
}

void test_allocate_exposes_writable_owned_payload ()
{
    zlink::message_t msg = zlink::message_t::allocate (3);
    assert (msg.valid ());
    assert (msg.size () == 3);
    assert (!msg.is_empty ());

    std::span<std::byte> bytes = msg.bytes ();
    bytes[0] = std::byte{0x01};
    bytes[1] = std::byte{0x02};
    bytes[2] = std::byte{0x03};

    const std::vector<uint8_t> out = msg.to_bytes ();
    assert ((out == std::vector<uint8_t>{0x01, 0x02, 0x03}));
}

void test_copy_helpers_copy_payload ()
{
    zlink::message_t empty;
    assert (empty.valid ());
    assert (empty.is_empty ());

    zlink::message_t msg = zlink::message_t::from ("copy-payload");
    assert (!msg.is_empty ());

    std::vector<uint8_t> target (msg.size ());
    assert (msg.copy_to (std::span<uint8_t> (target.data (), target.size ())) == msg.size ());
    assert (
      (target == std::vector<uint8_t>{'c', 'o', 'p', 'y', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd'}));

    bool threw = false;
    try {
        std::vector<uint8_t> too_small (msg.size () - 1);
        (void) msg.copy_to (std::span<uint8_t> (too_small.data (), too_small.size ()));
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    assert (threw);
}

void test_copy_and_move_preserve_payload ()
{
    const std::string payload (1024, 'c');
    zlink::message_t original = zlink::message_t::from (payload);
    zlink::message_t copy (original);
    zlink::message_t moved (std::move (original));

    assert (copy.valid ());
    assert (copy.to_string () == payload);
    assert (moved.valid ());
    assert (moved.to_string () == payload);
    assert (copy.ref_count () == 2);
    assert (moved.ref_count () == 2);
}

void test_large_owned_payload_survives_reuse_and_copy ()
{
    constexpr size_t payload_size = 262144;
    for (uint8_t marker : {uint8_t{0x2a}, uint8_t{0x5c}}) {
        zlink::message_t original (payload_size);
        assert (original.valid ());
        std::memset (original.data (), marker, payload_size);

        zlink::message_t copy (original);
        assert (copy.valid ());
        assert (copy.size () == payload_size);
        assert (std::to_integer<uint8_t> (copy.bytes ()[0]) == marker);
        assert (std::to_integer<uint8_t> (copy.bytes ()[payload_size - 1]) == marker);
    }
}

void test_external_message_uses_caller_owned_storage_until_close ()
{
    std::atomic<int> frees {0};
    auto free_payload = [] (void *data_, void *hint_) {
        delete[] static_cast<uint8_t *> (data_);
        static_cast<std::atomic<int> *> (hint_)->fetch_add (1);
    };

    uint8_t *payload = new uint8_t[4] {'z', 'e', 'r', 'o'};
    zlink::message_t msg = zlink::advanced::external_message_t::from (
      std::span<uint8_t> (payload, 4), free_payload, &frees);
    assert (msg.valid ());
    payload[0] = 'Z';
    assert ((msg.to_bytes () == std::vector<uint8_t>{'Z', 'e', 'r', 'o'}));
    assert (frees.load () == 0);
    msg.close ();
    assert (frees.load () == 1);
    msg.close ();
    assert (frees.load () == 1);
}

void test_diagnostic_surface_uses_canonical_names ()
{
    zlink::message_t msg = zlink::message_t::from ("diagnostic");
    assert (msg.valid ());
    assert (msg.ref_count () >= 1);
}

void test_routing_id_hex_and_display_policy ()
{
    const zlink::routing_id_t rid =
      zlink::routing_id_t::from (std::vector<uint8_t>{0x00, 0x41, 0x42});
    const zlink::routing_id_t parsed = zlink::routing_id_t::from_hex ("004142");

    assert (parsed == rid);
    assert (rid.to_hex () == "004142");
    assert (rid.to_string () == "hex:004142");
    assert (zlink::routing_id_t::from_hex (std::string (510, 'a')).size () == 255);

    bool threw = false;
    try {
        (void) zlink::routing_id_t::from_hex ("not-hex");
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    assert (threw);

    threw = false;
    try {
        (void) zlink::routing_id_t::from_hex (std::string (512, 'a'));
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    assert (threw);

    assert (zlink::routing_id_t::from (std::string ("dealer-1")).to_string () == "dealer-1");
    assert (zlink::routing_id_t::from (static_cast<uint32_t> (23)).to_string () == "23");
}

} // namespace

int main ()
{
    test_string_roundtrip ();
    test_bytes_roundtrip ();
    test_allocate_exposes_writable_owned_payload ();
    test_copy_helpers_copy_payload ();
    test_copy_and_move_preserve_payload ();
    test_large_owned_payload_survives_reuse_and_copy ();
    test_external_message_uses_caller_owned_storage_until_close ();
    test_diagnostic_surface_uses_canonical_names ();
    test_routing_id_hex_and_display_policy ();
    return 0;
}
