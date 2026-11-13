from pathlib import Path
r=Path('/root/worktree/zlink-cproto/framework/languages/cpp/framework')
p=r/'src/runtime/protocol/service_wire_codec.hpp';s=p.read_text().replace('#include <cstdint>','#include <cstdint>\n#include <functional>',1)
s=s.replace('struct application_payload_t\n{','using application_envelope_encoder_t = std::function<zlink::message_t ()>;\n\nstruct application_payload_t\n{',1)
s=s.replace('    static application_payload_t from_parts (const multipart_t &parts);','''    static application_payload_t from_envelope_encoder (application_envelope_encoder_t encode);
    const application_envelope_encoder_t *envelope_encoder () const noexcept
    {
        return std::get_if<application_envelope_encoder_t> (&_body);
    }
    static application_payload_t from_parts (const multipart_t &parts);''')
s=s.replace('        return std::get<byte_payload_t> (_body).bytes;','        materialize_envelope ();\n        return std::get<byte_payload_t> (_body).bytes;',1)
s=s.replace('    std::vector<std::uint8_t> &payload_bytes () { return std::get<byte_payload_t> (_body).bytes; }','    std::vector<std::uint8_t> &payload_bytes () { materialize_envelope (); return std::get<byte_payload_t> (_body).bytes; }')
s=s.replace('    void release_body ()\n    {','    void release_body ()\n    {\n        if (envelope_encoder ()) {\n            _body = multipart_t{};\n            return;\n        }',1)
s=s.replace('    std::variant<byte_payload_t, multipart_t> _body;','    void materialize_envelope () const;\n    mutable std::variant<byte_payload_t, multipart_t, application_envelope_encoder_t> _body;')
s=s.replace('zlink::message_t encode_application_payload_message (const application_payload_t &payload);','''zlink::message_t encode_application_payload_message (const application_payload_t &payload);
zlink::message_t encode_application_envelope_message (
  std::size_t header_size, const std::function<void (std::span<std::byte>)> &write_header,
  std::span<const std::byte> body);''');p.write_text(s)
p=r/'src/runtime/protocol/service_wire_codec.cpp';s=p.read_text();i=s.index('    zlink::message_t take () &&\n');s=s[:i]+'''    void generate (std::size_t size, const std::function<void (std::span<std::byte>)> &write)
    {
        if (size > _message.size () - _size)
            throw service_wire_error_t ("native wire writer exceeded allocated size");
        write ({reinterpret_cast<std::byte *> (end ()), size});
        _size += size;
    }
'''+s[i:]
s=s.replace('return parts () ? multipart_name','return parts () || envelope_encoder () ? multipart_name').replace('return parts () ? multipart_type','return parts () || envelope_encoder () ? multipart_type')
s=s.replace('    if (const auto *parts = other.parts ()) {\n        _body = retain_application_messages (*parts);','    if (const auto *encode = other.envelope_encoder ()) {\n        _body = *encode;\n    } else if (const auto *parts = other.parts ()) {\n        _body = retain_application_messages (*parts);',1)
i=s.index('application_payload_t application_payload_t::from_parts (const multipart_t &parts)');s=s[:i]+'''application_payload_t application_payload_t::from_envelope_encoder (
  application_envelope_encoder_t encode)
{
    application_payload_t result;
    result._body = std::move (encode);
    return result;
}

void application_payload_t::materialize_envelope () const
{
    if (const auto *encode = envelope_encoder ()) {
        auto frame = (*encode) ();
        auto decoded = decode_application_payload (message_bytes (frame));
        _body = std::move (decoded._body);
    }
}

'''+s[i:]
s=s.replace('template <typename Buffer>\nBuffer encode_application_payload_into (const application_payload_t &payload)','template <typename Buffer, typename Payload>\nBuffer encode_application_payload_into (const Payload &payload)',1)
a=s.index('Buffer encode_application_payload_into');b=s.index('\nnamespace\n{\ntemplate <typename RetainBody>',a);block=s[a:b]
block=block.replace('for (const auto &part : *parts) {','for (std::size_t index = 0; index < parts->size (); ++index) {\n            const auto &part = (*parts)[index];')
old='''            const auto bytes = part.bytes ();
            if (!bytes.empty ()) {
                const auto *begin = reinterpret_cast<const std::uint8_t *> (bytes.data ());
                result.insert (result.end (), begin, begin + bytes.size ());
            }'''
new='''            if constexpr (requires { part.append_to (result); }) {
                part.append_to (result);
            } else {
                const auto bytes = part.bytes ();
                if (!bytes.empty ()) {
                    const auto *begin = reinterpret_cast<const std::uint8_t *> (bytes.data ());
                    result.insert (result.end (), begin, begin + bytes.size ());
                }
            }'''
assert old in block;block=block.replace(old,new)
block=block.replace('    return encode_application_payload_into<std::vector<std::uint8_t>> (payload);','    if (const auto *encode = payload.envelope_encoder ())\n        return (*encode) ().to_bytes ();\n    return encode_application_payload_into<std::vector<std::uint8_t>> (payload);')
block=block.replace('    return encode_application_payload_into<native_wire_writer_t> (payload).take ();','    if (const auto *encode = payload.envelope_encoder ())\n        return (*encode) ();\n    return encode_application_payload_into<native_wire_writer_t> (payload).take ();')
s=s[:a]+block+s[b:];i=s.index('\nnamespace\n{\ntemplate <typename RetainBody>',a)
s=s[:i]+'''
namespace
{
struct envelope_part_t
{
    std::size_t length;
    const std::function<void (std::span<std::byte>)> *write;
    std::span<const std::byte> body;
    std::size_t size () const noexcept { return length; }
    void append_to (native_wire_writer_t &output) const
    {
        if (write)
            output.generate (length, *write);
        else if (!body.empty ()) {
            const auto *begin = reinterpret_cast<const std::uint8_t *> (body.data ());
            output.insert (output.end (), begin, begin + body.size ());
        }
    }
};
struct envelope_parts_t
{
    std::array<envelope_part_t, 2> messages;
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
    const auto *parts () const noexcept { return &messages; }
    const std::string &packet_name () const noexcept { return multipart_packet_name_text (); }
    const std::string &content_type () const noexcept { return multipart_content_type_text (); }
    std::span<const std::uint8_t> payload_bytes () const noexcept { return {}; }
};
}
zlink::message_t encode_application_envelope_message (
  std::size_t header_size, const std::function<void (std::span<std::byte>)> &write_header,
  std::span<const std::byte> body)
{
    envelope_parts_t payload{{envelope_part_t{header_size, &write_header, {}},
                              envelope_part_t{body.size (), nullptr, body}}, {}, {}};
    return encode_application_payload_into<native_wire_writer_t> (payload).take ();
}
'''+s[i:]
# Keep each static profile text in one place for both payload representations.
s=s.replace('    static const std::string multipart_name (framework_multipart_packet_name);\n','').replace('    static const std::string multipart_type (framework_multipart_content_type);\n','')
s=s.replace('? multipart_name :','? multipart_packet_name_text () :').replace('? multipart_type :','? multipart_content_type_text () :')
marker='const std::string &application_payload_t::packet_name () const noexcept'
helper='''namespace
{
const std::string &multipart_packet_name_text ()
{
    static const std::string value (framework_multipart_packet_name);
    return value;
}
const std::string &multipart_content_type_text ()
{
    static const std::string value (framework_multipart_content_type);
    return value;
}
}

''';s=s.replace(marker,helper+marker,1);p.write_text(s)
# Header values and body remain owned until the existing service pack point.
p=r/'src/runtime/messaging/envelope_codec.hpp';s=p.read_text().replace('#include <array>','#include <array>\n#include <functional>',1);i=s.index('namespace zlink::framework::runtime::messaging');s=s[:i]+'namespace zlink::framework::runtime::protocol { struct application_payload_t; }\n\n'+s[i:]
s=s.replace('    explicit message_parts_t (std::vector<zlink::message_t> parts);','''    message_parts_t (std::vector<zlink::message_t> parts);
    static message_parts_t from_envelope_encoder (std::function<zlink::message_t ()> encode);
    protocol::application_payload_t take_application_payload () &&;''')
s=s.replace('    std::variant<std::vector<zlink::message_t>,','    mutable std::variant<std::vector<zlink::message_t>,').replace('std::span<const std::span<const std::byte>>>\n      _parts;','std::span<const std::span<const std::byte>>, std::function<zlink::message_t ()>>\n      _parts;')
old='''    zlink::message_t encode_header (const envelope_header_t &header,
                                    const detail::request_type_plan_t &type,
                                    const detail::prepared_header_fragment_t &channel) const;'''
new='''    std::function<zlink::message_t ()> prepare_service_payload (
      envelope_header_t header, zlink::message_t body,
      std::shared_ptr<const detail::request_type_plan_t> type,
      std::shared_ptr<const detail::prepared_header_fragment_t> channel) const;'''
assert old in s;s=s.replace(old,new);p.write_text(s)
p=r/'src/runtime/messaging/envelope_codec.cpp';s=p.read_text().replace('#include "runtime/diagnostics/flow_context.hpp"','#include "runtime/diagnostics/flow_context.hpp"\n#include "runtime/protocol/service_wire_codec.hpp"',1)
i=s.index('message_parts_t::message_parts_t (const message_parts_t &other)');s=s[:i]+'''message_parts_t message_parts_t::from_envelope_encoder (std::function<zlink::message_t ()> encode)
{
    message_parts_t result;
    result._parts = std::move (encode);
    return result;
}
protocol::application_payload_t message_parts_t::take_application_payload () &&
{
    if (auto *encode = std::get_if<std::function<zlink::message_t ()>> (&_parts))
        return protocol::application_payload_t::from_envelope_encoder (std::move (*encode));
    return protocol::application_payload_t::from_parts (std::move (*this).take_items ());
}

'''+s[i:]
s=s.replace('    if (!std::holds_alternative<std::span<const std::span<const std::byte>>> (other._parts)) {','    if (const auto *encode = std::get_if<std::function<zlink::message_t ()>> (&other._parts)) {\n        _parts = *encode;\n        return;\n    }\n    if (!std::holds_alternative<std::span<const std::span<const std::byte>>> (other._parts)) {',1)
s=s.replace('          if constexpr (std::is_pointer_v<std::decay_t<decltype (parts)>>)','          if constexpr (std::is_same_v<std::decay_t<decltype (parts)>, std::function<zlink::message_t ()>>)\n              return std::size_t{2};\n          else if constexpr (std::is_pointer_v<std::decay_t<decltype (parts)>>)',1)
s=s.replace('const std::vector<zlink::message_t> &message_parts_t::items () const\n{','const std::vector<zlink::message_t> &message_parts_t::items () const\n{\n    if (auto *encode = std::get_if<std::function<zlink::message_t ()>> (&_parts))\n        _parts = protocol::decode_application_parts ((*encode) (), false);',1)
s=s.replace('std::vector<zlink::message_t> message_parts_t::take_items () &&\n{','std::vector<zlink::message_t> message_parts_t::take_items () &&\n{\n    if (auto *encode = std::get_if<std::function<zlink::message_t ()>> (&_parts))\n        return protocol::decode_application_parts ((*encode) (), false);',1)
a=s.index('zlink::message_t envelope_codec_t::encode_header (\n  const envelope_header_t &header, const detail::request_type_plan_t &type,');b=s.index('\nnamespace\n{\n\nenum class header_member_t',a)
s=s[:a]+'''std::function<zlink::message_t ()> envelope_codec_t::prepare_service_payload (
  envelope_header_t header, zlink::message_t body,
  std::shared_ptr<const detail::request_type_plan_t> type,
  std::shared_ptr<const detail::prepared_header_fragment_t> channel) const
{
    const auto [flow_id, origin] = validate_outgoing_header (header);
    if (channel->error)
        std::rethrow_exception (channel->error);
    if (type->error)
        std::rethrow_exception (type->error);
    // Preserve the E1 flow snapshot when target resolution defers the E3 pack.
    if (flow_id && !header.flow_id)
        header.flow_id = *flow_id;
    header.flow_origin = origin;
    header_writer_t measure;
    write_header (measure, header, header.flow_id ? &*header.flow_id : nullptr,
                  header.flow_origin, channel->bytes, type->middle, type->prefix);
    return [header = std::move (header), body = std::move (body), type = std::move (type),
            channel = std::move (channel), size = measure.size ()] {
        auto write = [&] (std::span<std::byte> destination) {
            header_writer_t writer (reinterpret_cast<char *> (destination.data ()));
            write_header (writer, header, header.flow_id ? &*header.flow_id : nullptr,
                          header.flow_origin, channel->bytes, type->middle, type->prefix);
        };
        return protocol::encode_application_envelope_message (size, std::ref (write), body.bytes ());
    };
}
'''+s[b:];p.write_text(s)
# Retain immutable plans from the existing cache and published bind index.
p=r/'include/zlink/framework/contracts/codecs/serializer.hpp';s=p.read_text();old='''    template <typename T> static const request_type_plan_t &request_plan (const serializer_t<T> &serializer) noexcept
    {
        return serializer._state->plan;
    }''';new='''    template <typename T>
    static std::shared_ptr<const request_type_plan_t> request_plan (const serializer_t<T> &serializer) noexcept
    {
        return {serializer._state, &serializer._state->plan};
    }''';assert old in s;s=s.replace(old,new);p.write_text(s)
p=r/'include/zlink/framework/contracts/channels/channel.hpp';s=p.read_text().replace('std::cref (detail::serializer_registry_access_t::request_plan (*selected_serializer))','detail::serializer_registry_access_t::request_plan (*selected_serializer)').replace('std::reference_wrapper<const detail::request_type_plan_t>','std::shared_ptr<const detail::request_type_plan_t>');p.write_text(s)
p=r/'src/runtime/channels/channel_runtime.cpp';s=p.read_text().replace('type_plan.get (),\n                                      endpoint->second.channel);','std::move (type_plan),\n            std::shared_ptr<const detail::prepared_header_fragment_t> (index, &endpoint->second.channel));');p.write_text(s)
p=r/'src/runtime/channels/channel_runtime.hpp';s=p.read_text().replace('is resolved on the lane.','is retained when the index is published.');p.write_text(s)
p=r/'src/runtime/messaging/client_call_codec.hpp';s=p.read_text();old='''    message_parts_t encode_payload_parts (const envelope_header_t &header,
                                          encoded_payload_t payload,
                                          const detail::request_type_plan_t &type,
                                          const detail::prepared_header_fragment_t &channel) const;''';new='''    message_parts_t encode_payload_parts (
      envelope_header_t header, encoded_payload_t payload,
      std::shared_ptr<const detail::request_type_plan_t> type,
      std::shared_ptr<const detail::prepared_header_fragment_t> channel) const;''';assert old in s;s=s.replace(old,new);p.write_text(s)
p=r/'src/runtime/messaging/client_call_codec.cpp';s=p.read_text();a=s.index('message_parts_t client_call_codec_t::encode_payload_parts (\n  const envelope_header_t &header, encoded_payload_t payload,');b=s.index('\nenvelope_header_t client_call_codec_t::create_envelope',a);s=s[:a]+'''message_parts_t client_call_codec_t::encode_payload_parts (
  envelope_header_t header, encoded_payload_t payload,
  std::shared_ptr<const detail::request_type_plan_t> type,
  std::shared_ptr<const detail::prepared_header_fragment_t> channel) const
{
    auto body = detail::encoded_payload_to_raw (std::move (payload));
    return message_parts_t::from_envelope_encoder (_codec.prepare_service_payload (
      std::move (header), std::move (body), std::move (type), std::move (channel)));
}
'''+s[b:];p.write_text(s)
# Forward the owned envelope representation until the original pack point.
p=r/'src/runtime/mesh/mesh_node_runtime.hpp';s=p.read_text()
for name in ['request_node_reply','submit_node_request_reply','request_unknown_node_reply']:
 a=s.index(name+' (');b=s.index(';',a);s=s[:a]+s[a:b].replace('std::vector<zlink::message_t> parts','runtime::messaging::message_parts_t parts')+s[b:]
p.write_text(s)
p=r/'src/runtime/mesh/mesh_node_runtime.cpp';s=p.read_text()
for name in ['submit_node_request_reply','request_node_reply','request_unknown_node_reply']:
 a=s.index('void mesh_node_runtime_t::'+name);b=s.index('\n}',a);s=s[:a]+s[a:b].replace('std::vector<zlink::message_t> parts','runtime::messaging::message_parts_t parts')+s[b:]
p.write_text(s)
p=r/'src/runtime/stateful/public_host_runtime.hpp';s=p.read_text();a=s.index('    request_to_node (const zlink::routing_id_t &target,');b=s.index(';',a);s=s[:a]+s[a:b].replace('std::vector<zlink::message_t> parts','runtime::messaging::message_parts_t parts')+s[b:];p.write_text(s)
p=r/'src/runtime/stateful/public_host_runtime.cpp';s=p.read_text();a=s.index('public_host_runtime_t::request_to_node (const zlink::routing_id_t &target,',s.index('public_host_runtime_t::request_to_node (const zlink::routing_id_t &target,')+1);b=s.index('\ntask_t<zlink::submit_result_t>',a);block=s[a:b].replace('std::vector<zlink::message_t> parts','runtime::messaging::message_parts_t parts').replace('encode_application (std::move (parts))','std::move (parts).take_application_payload ()');s=s[:a]+block+s[b:];p.write_text(s)
p=r/'src/runtime/host/app.cpp';s=p.read_text().replace('mesh->request_node_reply (target, std::move (parts).take_items (), timeout,','mesh->request_node_reply (target, std::move (parts), timeout,');p.write_text(s)
t=r.parent/'tests/Zlink.Framework.UnitTests'
for name in ['test_cpp_framework_request_native_counts.cpp',
             'test_cpp_framework_request_transport.cpp',
             'test_cpp_framework_request_transport_projection.cpp']:
    p=t/name;s=p.read_text()
    s=s.replace('request_node_reply (rid, std::move (parts).take_items (), timeout,',
                'request_node_reply (rid, std::move (parts), timeout,')
    s=s.replace('request_node_reply (target, std::move (parts).take_items (),',
                'request_node_reply (target, std::move (parts),')
    if name=='test_cpp_framework_request_native_counts.cpp':
        s=s.replace('constexpr std::size_t closes_per_request = 10;',
                    'constexpr std::size_t closes_per_request = 9;')
        s=s.replace('''    // Binding retains two native send views. Framework adds two inner parts
    // and two service parts; the receive owner initializes two adoption slots.''',
                    '''    // Binding retains two native send views. Framework creates one typed body
    // and two service parts; the receive owner initializes two adoption slots.
    // The JSON header is written directly into the service payload frame.''')
        s=s.replace('EXPECT_EQ (calls[0] + calls[1] + calls[2], requests * 8);',
                    'EXPECT_EQ (calls[0] + calls[1] + calls[2], requests * 7);')
        s=s.replace('EXPECT_EQ (calls[4], requests * 10);',
                    'EXPECT_EQ (calls[4], requests * 9);')
    p.write_text(s)
# A RouteChannel sends its existing two parts; service transports fuse the header.
# Select that immutable codec plan at bind, rather than adding a caller branch.
p=r/'src/runtime/messaging/envelope_codec.hpp';s=p.read_text();marker='    std::function<zlink::message_t ()> prepare_service_payload (';s=s.replace(marker,'''    zlink::message_t encode_header (const envelope_header_t &header,
                                    const detail::request_type_plan_t &type,
                                    const detail::prepared_header_fragment_t &channel) const;
'''+marker,1);p.write_text(s)
p=r/'src/runtime/messaging/envelope_codec.cpp';s=p.read_text();marker='} // namespace\n\nzlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header) const'
helper='''std::pair<const std::string *, std::optional<flow_origin_t>>
validate_request_header (const envelope_header_t &header,
                         const detail::request_type_plan_t &type,
                         const detail::prepared_header_fragment_t &channel)
{
    const auto flow = validate_outgoing_header (header);
    if (channel.error)
        std::rethrow_exception (channel.error);
    if (type.error)
        std::rethrow_exception (type.error);
    return flow;
}

''';assert marker in s;s=s.replace(marker,helper+marker,1)
s=s.replace('''    const auto [flow_id, origin] = validate_outgoing_header (header);
    if (channel->error)
        std::rethrow_exception (channel->error);
    if (type->error)
        std::rethrow_exception (type->error);''','''    const auto [flow_id, origin] = validate_request_header (header, *type, *channel);''',1)
i=s.index('std::function<zlink::message_t ()> envelope_codec_t::prepare_service_payload (')
s=s[:i]+'''zlink::message_t envelope_codec_t::encode_header (
  const envelope_header_t &header, const detail::request_type_plan_t &type,
  const detail::prepared_header_fragment_t &channel) const
{
    const auto [flow_id, origin] = validate_request_header (header, type, channel);
    header_writer_t measure;
    write_header (measure, header, flow_id, origin, channel.bytes, type.middle, type.prefix);
    auto message = zlink::message_t::allocate (measure.size ());
    if (!message.valid ())
        throw std::bad_alloc ();
    header_writer_t writer (reinterpret_cast<char *> (message.data ()));
    write_header (writer, header, flow_id, origin, channel.bytes, type.middle, type.prefix);
    return message;
}

'''+s[i:];p.write_text(s)
p=r/'src/runtime/messaging/client_call_codec.hpp';s=p.read_text();old='''    message_parts_t encode_payload_parts (
      envelope_header_t header, encoded_payload_t payload,
      std::shared_ptr<const detail::request_type_plan_t> type,
      std::shared_ptr<const detail::prepared_header_fragment_t> channel) const;''';assert old in s;s=s.replace(old,old+'\n'+old.replace('encode_payload_parts (','encode_service_payload_parts ('),1);p.write_text(s)
p=r/'src/runtime/messaging/client_call_codec.cpp';s=p.read_text();key='message_parts_t client_call_codec_t::encode_payload_parts (\n  envelope_header_t header, encoded_payload_t payload,';assert key in s;s=s.replace(key,key.replace('encode_payload_parts (','encode_service_payload_parts ('),1);i=s.index('message_parts_t client_call_codec_t::encode_service_payload_parts (');s=s[:i]+'''message_parts_t client_call_codec_t::encode_payload_parts (
  envelope_header_t header, encoded_payload_t payload,
  std::shared_ptr<const detail::request_type_plan_t> type,
  std::shared_ptr<const detail::prepared_header_fragment_t> channel) const
{
    auto body = detail::encoded_payload_to_raw (std::move (payload));
    return message_parts_t (_codec.encode_header (header, *type, *channel), std::move (body));
}

'''+s[i:];p.write_text(s)
p=r/'src/runtime/channels/channel_runtime.hpp';s=p.read_text().replace('#include "runtime/messaging/envelope_codec.hpp"','#include "runtime/messaging/envelope_codec.hpp"\n#include "runtime/messaging/client_call_codec.hpp"',1)
old='''        struct entry_t
        {
            node_transport_t transport;
            prepared_header_fragment_t channel;
        };''';new='''        struct entry_t
        {
            using request_encoder_t = runtime::messaging::message_parts_t
              (runtime::messaging::client_call_codec_t::*) (
                runtime::messaging::envelope_header_t, encoded_payload_t,
                std::shared_ptr<const request_type_plan_t>,
                std::shared_ptr<const prepared_header_fragment_t>) const;
            node_transport_t transport;
            prepared_header_fragment_t channel;
            request_encoder_t encode_request =
              &runtime::messaging::client_call_codec_t::encode_payload_parts;
        };''';assert old in s;s=s.replace(old,new,1);p.write_text(s)
p=r/'src/runtime/channels/channel_runtime.cpp';s=p.read_text();s=s.replace('transport, prepare_request_channel (mesh_name)}','transport, prepare_request_channel (mesh_name),\n                      &runtime::messaging::client_call_codec_t::encode_service_payload_parts}')
s=s.replace('codec.encode_payload_parts (std::move (header), std::move (payload), std::move (type_plan),','(codec.*endpoint->second.encode_request) (std::move (header), std::move (payload), std::move (type_plan),',1);p.write_text(s)
