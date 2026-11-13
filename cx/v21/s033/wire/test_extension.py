from pathlib import Path
p=Path('/root/worktree/zlink-3867w/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_service_wire_codec.cpp')
s=p.read_text();marker='// Round 3 baseline:'
test='''// Existing codec accessors expose both container and native payload identity.
static void test_moved_multipart_ownership ()
{
    protocol::application_payload_t::multipart_t parts;
    parts.push_back (zlink::message_t::from (std::vector<std::uint8_t> (1024, 0xa5)));
    parts.push_back (zlink::message_t::from (std::vector<std::uint8_t> (2048, 0x5a)));
    const auto *items = parts.data ();
    const auto *first = parts[0].data ();
    const auto *second = parts[1].data ();
    const auto first_refs = parts[0].ref_count ();
    auto payload = protocol::application_payload_t::from_parts (std::move (parts));
    assert (parts.empty ());
    assert (payload.parts ()->data () == items);
    assert ((*payload.parts ())[0].data () == first);
    assert ((*payload.parts ())[1].data () == second);
    assert ((*payload.parts ())[0].ref_count () == first_refs);
    // A retained payload has its own list and shares only the binding storage.
    // A shared multipart wrapper fails this assertion.
    auto retained = payload;
    assert (retained.parts () != payload.parts ());
    assert (retained.parts ()->data () != items);
    assert ((*retained.parts ())[0].data () == first);
    payload = {};
    assert ((*retained.parts ())[0].to_bytes () == std::vector<std::uint8_t> (1024, 0xa5));
}

'''
s=s.replace(marker,test+marker)
s=s.replace('    test_retained_multipart_wire_bytes ();','    test_retained_multipart_wire_bytes ();\n    test_moved_multipart_ownership ();')
p.write_text(s)
