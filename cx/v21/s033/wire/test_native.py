from pathlib import Path
p=Path('/root/worktree/zlink-3867w/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_service_wire_codec.cpp')
s=p.read_text();marker='            const auto in_flight = retained;'
s=s.replace(marker,'''            assert (protocol::encode_node_request_header_message (0x0102030405060708).to_bytes () == request);
            assert (protocol::encode_reply_header_message (0x0102030405060708, 0, 0).to_bytes () == reply);
            assert (protocol::encode_node_send_header_message ().to_bytes () == send);
            const auto native_payload = protocol::encode_application_payload_message (retained);
            assert (native_payload.to_bytes () == expected);
            for (const auto &head : {request, reply, send}) {
                const std::vector<std::vector<std::uint8_t>> expected_wire{head, expected};
                std::vector<std::vector<std::uint8_t>> actual_wire{head, native_payload.to_bytes ()};
                assert (actual_wire == expected_wire);
                actual_wire[1].back () ^= 1;
                assert (actual_wire != expected_wire);
            }
'''+marker)
# Pin flow v2 output through the same frozen expectation, including invalid pair rejection.
s=s.replace('    assert (protocol::decode_application_payload (traced_application_wire) == traced_application);', '    assert (protocol::decode_application_payload (traced_application_wire) == traced_application);\n    assert (protocol::encode_application_payload_message (traced_application).to_bytes () == traced_application_wire);')
p.write_text(s)
