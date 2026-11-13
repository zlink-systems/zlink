from pathlib import Path
p=Path('/root/jobs/fx/cx/v21/s033/cproto/step2.py');s=p.read_text();i=s.rindex('p.write_text(s)')
extra=r'''
old = ''' + repr('''    const auto &flow = flow_context_t::current ();
    const bool stamp_flow = !header.flow_id && flow && !flow->flow_id.empty ();
    const auto *flow_id = stamp_flow ? &flow->flow_id : header.flow_id ? &*header.flow_id : nullptr;
    const auto flow_origin =
      stamp_flow ? std::optional<flow_origin_t> (flow->origin) : header.flow_origin;
    if (auto valid =
          validate_protocol_header (header, flow_id_t::format_marker, flow_id, flow_origin);
        !valid) {
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());
    }
''')+r'''
assert old in s
s = s.replace(old, '    const auto [flow_id, flow_origin] = validate_outgoing_header (header);\n', 1)
old = '''+repr('''    const auto &flow = flow_context_t::current ();
    const bool stamp_flow = !header.flow_id && flow && !flow->flow_id.empty ();
    const auto *flow_id = stamp_flow ? &flow->flow_id : header.flow_id ? &*header.flow_id : nullptr;
    const auto origin = stamp_flow ? std::optional<flow_origin_t> (flow->origin) : header.flow_origin;
    if (auto valid = validate_protocol_header (header, flow_id_t::format_marker, flow_id, origin);
        !valid)
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());
''')+r'''
assert old in s
s = s.replace(old, '    const auto [flow_id, origin] = validate_outgoing_header (header);\n', 1)
marker = '} // namespace\n\nzlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header) const'
helper = '''+repr('''std::pair<const std::string *, std::optional<flow_origin_t>>
validate_outgoing_header (const envelope_header_t &header)
{
    const auto &flow = flow_context_t::current ();
    const bool stamp = !header.flow_id && flow && !flow->flow_id.empty ();
    const auto *id = stamp ? &flow->flow_id : header.flow_id ? &*header.flow_id : nullptr;
    const auto origin = stamp ? std::optional<flow_origin_t> (flow->origin) : header.flow_origin;
    if (auto valid = validate_protocol_header (header, flow_id_t::format_marker, id, origin); !valid)
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());
    return {id, origin};
}

''')+r'''
assert marker in s
s = s.replace(marker, helper + marker, 1)
'''
p.write_text(s[:i]+extra+s[i:])
