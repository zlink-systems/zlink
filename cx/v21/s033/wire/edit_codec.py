from pathlib import Path
root=Path('/root/worktree/zlink-3867w/framework/languages/cpp/framework/src/runtime')
p=root/'protocol/service_wire_codec.hpp'; s=p.read_text()
s=s.replace('    application_payload_t () = default;', '''    application_payload_t () = default;
    application_payload_t (const application_payload_t &other);
    application_payload_t &operator= (const application_payload_t &other);
    application_payload_t (application_payload_t &&) noexcept = default;
    application_payload_t &operator= (application_payload_t &&) noexcept = default;''')
s=s.replace('''        const auto value = std::get_if<std::shared_ptr<const multipart_t>> (&_body);
        return value ? value->get () : nullptr;''','''        return std::get_if<multipart_t> (&_body);''')
s=s.replace('std::variant<std::vector<std::uint8_t>, std::shared_ptr<const multipart_t>> _body;', 'std::variant<std::vector<std::uint8_t>, multipart_t> _body;')
names=['node_send_header','node_request_header','channel_request_header','channel_send_header','spot_message_header','actor_message_header','reply_header','application_payload']
import re
for name in names:
    pattern=r'std::vector<std::uint8_t>\s+encode_'+name+r'\s*\([^;]+;'
    decl=re.search(pattern,s).group()
    native=re.sub(r'std::vector<std::uint8_t>\s+', 'zlink::message_t ',decl,count=1).replace('encode_'+name,'encode_'+name+'_message')
    s=s.replace(decl,decl+'\n'+native)
p.write_text(s)
p=root/'protocol/service_wire_codec.cpp'; s=p.read_text(); s=s.replace('#include <array>','#include <array>\n#include <cassert>')
insert='''// Sized by the codec before the first write; the binding message owns the bytes.
class native_wire_writer_t
{
  public:
    void reserve (std::size_t size) { _message = zlink::message_t::allocate (size); }
    void push_back (std::uint8_t value)
    {
        assert (_size < _message.size ());
        reinterpret_cast<std::uint8_t *> (_message.data ())[_size++] = value;
    }
    std::uint8_t *end () { return reinterpret_cast<std::uint8_t *> (_message.data ()) + _size; }
    template <typename Iterator> void insert (std::uint8_t *, Iterator first, Iterator last)
    {
        const auto count = static_cast<std::size_t> (std::distance (first, last));
        assert (_size + count <= _message.size ());
        std::copy (first, last, end ());
        _size += count;
    }
    void insert (std::uint8_t *position, std::initializer_list<std::uint8_t> bytes)
    {
        insert (position, bytes.begin (), bytes.end ());
    }
    zlink::message_t take () &&
    {
        assert (_size == _message.size ());
        return std::move (_message);
    }
  private:
    zlink::message_t _message;
    std::size_t _size = 0;
};

'''
s=s.replace('void append_u32 (std::vector<std::uint8_t> &bytes,',insert+'template <typename Buffer>\nvoid append_u32 (Buffer &bytes,',1)
for name in ['u16','u64','text8','bytes8']:
    s=s.replace('void append_'+name+' (std::vector<std::uint8_t> &bytes,','template <typename Buffer>\nvoid append_'+name+' (Buffer &bytes,')
# Inline value ownership; copies retain native storage via the existing binding copy operation.
start=s.index('application_payload_t application_payload_t::from_parts (const multipart_t &parts)')
s=s[:start]+'''namespace
{
application_payload_t::multipart_t retain_application_messages (std::span<const zlink::message_t> parts)
{
    application_payload_t::multipart_t retained;
    retained.reserve (parts.size ());
    for (const auto &part : parts)
        retained.push_back (part.copy ());
    return retained;
}
} // namespace

application_payload_t::application_payload_t (const application_payload_t &other) :
    packet_name (other.packet_name), content_type (other.content_type),
    flow_id (other.flow_id), flow_origin (other.flow_origin)
{
    if (const auto *parts = other.parts ()) {
        _body = retain_application_messages (*parts);
    } else {
        _body = other.payload_bytes ();
    }
}

application_payload_t &application_payload_t::operator= (const application_payload_t &other)
{
    if (this != &other)
        *this = application_payload_t (other);
    return *this;
}

'''+s[start:]
s=s.replace('auto retained = std::make_shared<multipart_t> ();\n    retained->reserve (parts.size ());','multipart_t retained;\n    retained.reserve (parts.size ());')
s=s.replace('retained->push_back (part.copy ());','retained.push_back (part.copy ());')
s=s.replace('result._body = std::make_shared<multipart_t> (std::move (parts));','result._body = std::move (parts);')
s=s.replace('''    multipart_t retained;
    retained.reserve (parts.size ());
    for (const auto &part : parts)
        retained.push_back (part.copy ());
    application_payload_t result;''', '''    auto retained = retain_application_messages (parts);
    application_payload_t result;''')
# Transform the existing byte encoders to one encoding implementation with two storage sinks.
sizes={
'node_send_header':'prefix_size',
'node_request_header':'prefix_size + 8',
'channel_request_header':'prefix_size + 8 + 1 + channel_name.size ()',
'channel_send_header':'prefix_size + 1 + channel_name.size ()',
'spot_message_header':'prefix_size + (correlation ? 8 : 0) + 16 + 1 + 1 + source_spot_id.size () + 1 + target.spot_id.size () + 8 + 1 + target.target_node_routing_id.size () + 24',
'actor_message_header':'prefix_size + (correlation ? 8 : 0) + 16 + 1 + 1 + (source_actor ? source_actor->first.size () + 8 : 0) + 1 + target.actor_id.size () + 8 + 1 + target.target_node_routing_id.size () + 24 + (bound_session_source ? 1 + bound_session_source->session_routing_id.size () + 16 : 0)',
}
args={
'node_send_header':'', 'node_request_header':'correlation',
'channel_request_header':'correlation, channel_name','channel_send_header':'channel_name',
'spot_message_header':'kind, source_spot_id, target, operation, correlation, message_follow_hop_count',
'actor_message_header':'kind, source_actor, target, operation, correlation, message_follow_hop_count, bound_session_source',
'reply_header':'correlation, terminal_result, failure_code', 'application_payload':'payload'}
for name in names:
    pat=r'std::vector<std::uint8_t>\s+encode_'+name+r'\s*\(([^;]*?)\)\n\{'
    match=re.search(pat,s); begin=match.start(); brace=match.end()-1; level=1; end=brace+1
    while level:
        if s[end]=='{': level+=1
        if s[end]=='}': level-=1
        end+=1
    body=s[brace+1:end-1]
    if name=='node_send_header':
        body='\n    Buffer result;\n    result.reserve (prefix_size);\n    result.insert (result.end (), {magic[0], magic[1], wire_major, static_cast<std::uint8_t> (command::nodeSend), 0});\n    return result;\n'
    else:
        body=body.replace('std::vector<std::uint8_t> result;', 'Buffer result;')
        body=re.sub(r'std::vector<std::uint8_t> result\{(.*?)\};', lambda m:'Buffer result;\n    result.reserve ('+sizes[name]+');\n    result.insert (result.end (), {'+m.group(1)+'});',body,flags=re.S)
    params=match.group(1)
    impl='template <typename Buffer>\nBuffer encode_'+name+'_into ('+params+')\n{'+body+'}'
    wrappers='\n\nstd::vector<std::uint8_t> encode_'+name+' ('+params+')\n{\n    return encode_'+name+'_into<std::vector<std::uint8_t>> ('+args[name]+');\n}\n\nzlink::message_t encode_'+name+'_message ('+params+')\n{\n    return encode_'+name+'_into<native_wire_writer_t> ('+args[name]+').take ();\n}'
    s=s[:begin]+impl+wrappers+s[end:]
p.write_text(s)
