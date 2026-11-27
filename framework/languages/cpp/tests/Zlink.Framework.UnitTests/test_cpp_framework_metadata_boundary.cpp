/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "channel-header-v1.pb.h"
#include <zlink/framework/contracts/errors/error.hpp>

#include <gtest/gtest.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace mesh = zlink::framework::detail;
namespace messaging = zlink::framework::runtime::messaging;

bool check_boundary (std::size_t value_size, bool accepted)
{
    const std::string value (value_size, 'x');
    const std::map<std::string, std::string> metadata{{"k", value}};
    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::command;
    header.channel_name = "metadata";
    header.message_name = "metadata.send";
    header.metadata = {{"k", value}};
    messaging::envelope_codec_t codec;
    bool header_encoded = true;
    try {
        (void) codec.encode_header (header);
    }
    catch (const zlink::framework::framework_exception_t &) {
        header_encoded = false;
    }
    bool ok = header_encoded == accepted;

    zlink::framework::channel::v1::ChannelHeader wire;
    wire.set_kind (zlink::framework::channel::v1::COMMAND);
    wire.set_channel_name ("metadata");
    wire.set_message_name ("metadata.send");
    wire.set_content_type ("application/json");
    auto *entry = wire.add_metadata ();
    entry->set_key ("k");
    entry->set_value (value);
    EXPECT_GT (wire.ByteSizeLong (), value_size);
    const bool header_decoded =
      static_cast<bool> (codec.decode_header (zlink::message_t::from (wire.SerializeAsString ())));
    ok &= header_decoded == accepted;

    bool frame_encoded = true;
    try {
        (void) mesh::mesh_metadata_codec_t::encode (metadata);
    }
    catch (const zlink::framework::framework_exception_t &) {
        frame_encoded = false;
    }
    ok &= frame_encoded == accepted;
    std::vector<std::uint8_t> frame{1,
                                    1,
                                    1,
                                    'k',
                                    static_cast<std::uint8_t> (value_size >> 8),
                                    static_cast<std::uint8_t> (value_size)};
    frame.insert (frame.end (), value.begin (), value.end ());
    std::map<std::string, std::string> decoded;
    const bool frame_decoded = mesh::mesh_metadata_codec_t::decode (frame, decoded);
    ok &= frame_decoded == accepted;
    std::cout << "field13=" << value_size + 9 << " expected=" << accepted
              << " header-encode=" << header_encoded << " header-decode=" << header_decoded
              << " frame-encode=" << frame_encoded << " frame-decode=" << frame_decoded
              << std::endl;
    return ok;
}

TEST (MetadataBoundary, Field13Boundaries)
{
    EXPECT_TRUE (check_boundary (1017, false));
    EXPECT_TRUE (check_boundary (1015, true));
    EXPECT_TRUE (check_boundary (1016, false));
}

TEST (MetadataBoundary, EmptyValue)
{
    for (const std::size_t value_size : {1009u, 1010u}) {
        messaging::envelope_header_t header;
        header.kind = messaging::message_kind_t::command;
        header.channel_name = "metadata";
        header.message_name = "metadata.send";
        header.metadata = {{"é", ""}, {"k", std::string (value_size, 'x')}};
        zlink::framework::channel::v1::ChannelHeader wire;
        const auto base = wire.ByteSizeLong ();
        for (const auto &[key, value] : header.metadata) {
            auto *entry = wire.add_metadata ();
            entry->set_key (key);
            entry->set_value (value);
        }
        EXPECT_EQ (wire.ByteSizeLong () - base, value_size == 1009 ? 1024u : 1025u);
        wire.set_kind (zlink::framework::channel::v1::COMMAND);
        wire.set_channel_name ("metadata");
        wire.set_message_name ("metadata.send");
        wire.set_content_type ("application/json");
        messaging::envelope_codec_t codec;
        const bool accepted = value_size == 1009;
        EXPECT_EQ (static_cast<bool> (
                     codec.decode_header (zlink::message_t::from (wire.SerializeAsString ()))),
                   accepted);
        const std::map<std::string, std::string> metadata{{"é", ""},
                                                          {"k", std::string (value_size, 'x')}};
        if (accepted) {
            EXPECT_NO_THROW (codec.encode_header (header));
            EXPECT_NO_THROW (mesh::mesh_metadata_codec_t::encode (metadata));
        } else {
            EXPECT_THROW (codec.encode_header (header), zlink::framework::framework_exception_t);
            EXPECT_THROW (mesh::mesh_metadata_codec_t::encode (metadata),
                          zlink::framework::framework_exception_t);
        }
        std::vector<std::uint8_t> frame{1,
                                        2,
                                        1,
                                        'k',
                                        static_cast<std::uint8_t> (value_size >> 8),
                                        static_cast<std::uint8_t> (value_size)};
        frame.insert (frame.end (), value_size, 'x');
        frame.insert (frame.end (), {2, 0xc3, 0xa9, 0, 0});
        std::map<std::string, std::string> decoded;
        EXPECT_EQ (mesh::mesh_metadata_codec_t::decode (frame, decoded), accepted);
    }
}
