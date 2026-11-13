from pathlib import Path
r=Path('/root/worktree/zlink-3867w/framework/languages/cpp/framework/src/runtime')
p=r/'backend/raw_binding_adapter.hpp';s=p.read_text();i=s.index('inline std::vector<zlink::message_t> materialize_binding_parts')
s=s[:i]+'''inline zlink::message_t materialize_binding_part (zlink::message_t message)
{
    return message;
}

'''+s[i:];p.write_text(s)
p=r/'backend/raw_route_port.hpp';s=p.read_text()
# overload the two physical submit entry points, using one implementation for each
for name in ['submit_send','request']:
    start=s.index('    '+('raw_send_submission_t' if name=='submit_send' else 'void')+' '+name+' (')
    end=s.index(';',start)+1
    decl=s[start:end]
    s=s[:end]+'\n'+decl.replace('raw_message_t parts','std::vector<zlink::message_t> parts')+s[end:]
priv=s.index('  private:')
s=s[:priv]+s[priv:].replace('  private:', '''  private:
    template <typename Parts>
    raw_send_submission_t submit_send_impl (const raw_bytes_t &target_routing_id, Parts parts,
                                            raw_send_stage_trace_t trace);
    template <typename Parts>
    void request_impl (const raw_bytes_t &target_routing_id, Parts parts,
                       std::chrono::milliseconds timeout,
                       std::function<void (result_t<raw_request_completion_t>)> completion,
                       runtime::mesh::service_liveness_registry_t::connection_t admission,
                       std::shared_ptr<task_completion_source_t<void>> admitted);
''',1);p.write_text(s)
p=r/'backend/raw_route_port.cpp';s=p.read_text()
for name,ret,params,args in [
('submit_send','raw_send_submission_t','raw_send_stage_trace_t trace','std::move (trace)'),
('request','void','''std::chrono::milliseconds timeout,
                                std::function<void (result_t<raw_request_completion_t>)> completion,
                                runtime::mesh::service_liveness_registry_t::connection_t admission,
                                std::shared_ptr<task_completion_source_t<void>> admitted''','timeout, std::move (completion), std::move (admission), std::move (admitted)')]:
    start=s.index(ret+' raw_route_port_t::'+name+' (')
    end=s.index('\n{',start)
    signature=s[start:end]
    impl='template <typename Parts>\n'+signature.replace('::'+name+' (','::'+name+'_impl (').replace('raw_message_t parts','Parts parts')
    wrappers=''
    for typ in ['raw_message_t','std::vector<zlink::message_t>']:
        wrappers+=ret+' raw_route_port_t::'+name+' (const raw_bytes_t &target_routing_id, '+typ+' parts, '+params+')\n{\n    return '+name+'_impl (target_routing_id, std::move (parts), '+args+');\n}\n\n'
    s=s[:start]+wrappers+impl+s[end:]
p.write_text(s)
# Owner accepts native messages on common application paths. Infrastructure keeps its vector codec API.
for filename in ['mesh/raw_mesh_node_owner.hpp','mesh/raw_mesh_node_owner.cpp']:
 p=r/filename;s=p.read_text()
 for name in ['start_send','start_send_on_lane','submit_owned_send_result','send_with_header_result','request_with_header']:
    import re
    # signatures only
    pat=r'(?:raw_mesh_node_owner_t::)?'+name+r'\s*\([^;]+?(?=\n(?:\{|try \{)|;)'
    for m in list(re.finditer(pat,s))[::-1]:
        sig=m.group()
        if name=='request_with_header':
            sig=sig.replace('std::function<std::vector<std::uint8_t> (std::uint64_t)>','std::function<zlink::message_t (std::uint64_t)>')
        elif name=='send_with_header_result':
            sig=sig.replace('std::vector<std::uint8_t> header','zlink::message_t header')
        else: sig=sig.replace('detail::backend::raw_message_t parts','std::vector<zlink::message_t> parts')
        s=s[:m.start()]+sig+s[m.end():]
 if filename.endswith('cpp'):
    # Admission algorithms are shared, independent of the owning message container.
    s=s.replace('bool start_admitted_request (','template <typename Parts>\nbool start_admitted_request (')
    s=s.replace('detail::backend::raw_message_t &request_parts,','Parts &request_parts,')
    s=s.replace('template <typename TTerminal>\ntask_t<void> run_request_admission','template <typename TTerminal, typename Parts>\ntask_t<void> run_request_admission')
    s=s.replace('template <typename TTerminal>\ntask_t<bool> submit_registered_request','template <typename TTerminal, typename Parts>\ntask_t<bool> submit_registered_request')
    s=s.replace('detail::backend::raw_message_t request_parts,\n  std::function','Parts request_parts,\n  std::function')
    # Common application header callbacks now produce native output.
    a=s.index('task_t<bool> raw_mesh_node_owner_t::request_with_header'); b=s.index('struct raw_mesh_node_owner_t::send_completion_state_t',a)
    chunk=s[a:b].replace('protocol::encode_application_payload (','protocol::encode_application_payload_message (').replace('detail::backend::raw_message_t wire;','std::vector<zlink::message_t> wire;')
    s=s[:a]+chunk+s[b:]
    # Change callsites for application encoder headers; keep infrastructure byte headers intact.
    for name in ['node_send_header','node_request_header','channel_send_header','channel_request_header','spot_message_header','actor_message_header']:
        s=s.replace('protocol::encode_'+name+' (','protocol::encode_'+name+'_message (')
    a=s.index('raw_mesh_node_owner_t::send_with_header_result');b=s.index('task_t<zlink::submit_result_t>\nraw_mesh_node_owner_t::submit_owned_send_result',a)
    chunk=s[a:b].replace('detail::backend::raw_message_t parts;','std::vector<zlink::message_t> parts;').replace('parts.push_back (std::move (*metadata));','parts.push_back (zlink::message_t::from (*metadata));').replace('protocol::encode_application_payload (','protocol::encode_application_payload_message (')
    s=s[:a]+chunk+s[b:]
    # Main application reply
    a=s.index('bool raw_mesh_node_owner_t::reply ('); b=s.index('bool raw_mesh_node_owner_t::reply_failure',a)
    chunk=s[a:b].replace('detail::backend::raw_message_t parts;','std::vector<zlink::message_t> parts;').replace('protocol::encode_reply_header (','protocol::encode_reply_header_message (').replace('protocol::encode_application_payload (','protocol::encode_application_payload_message (')
    s=s[:a]+chunk+s[b:]
    # Logical multicast shares the same native send owner; its generated header remains a byte API.
    a=s.index('raw_mesh_node_owner_t::send_logical_multicast');b=s.index('raw_mesh_node_owner_t::request_to_node',a)
    chunk=s[a:b].replace('detail::backend::raw_message_t parts;','std::vector<zlink::message_t> parts;').replace('parts.push_back (std::move (header.data));','parts.push_back (detail::backend::materialize_binding_part (std::move (header.data)));').replace('parts.push_back (metadata);','parts.push_back (zlink::message_t::from (metadata));').replace('protocol::encode_application_payload (','protocol::encode_application_payload_message (')
    s=s[:a]+chunk+s[b:]
    s=s.replace('start_send (target_routing_id, detail::backend::raw_message_t{std::move (header)}, true)', 'start_send (target_routing_id, detail::backend::materialize_binding_parts ({std::move (header)}), true)')
    s=s.replace('target_routing_id, detail::backend::raw_message_t{std::move (header)}, false, {}','target_routing_id, detail::backend::materialize_binding_parts ({std::move (header)}), false, {}')
    # remote infrastructure send_control call -> native at boundary
    a=s.index('task_t<bool> raw_mesh_node_owner_t::send_control');b=s.index('\ntask_t<bool>',a+15)
    chunk=s[a:b].replace('submit_owned_send_result (target_routing_id, std::move (parts),','submit_owned_send_result (target_routing_id, detail::backend::materialize_binding_parts (std::move (parts)),')
    s=s[:a]+chunk+s[b:]
    # Activation's generated header has its own byte-vector API.
    s=s.replace('target_routing_id, protocol::encode_instance_spot_activation_header (request),','target_routing_id, detail::backend::materialize_binding_part (protocol::encode_instance_spot_activation_header (request)),')
 p.write_text(s)
# Consume dispatcher reply ownership through the existing rvalue message_parts seam.
for name in ['channels/route_packet_dispatcher.hpp','channels/route_packet_dispatcher.cpp','mesh/mesh_record_dispatcher.cpp']:
 p=r/name;s=p.read_text().replace('const route_dispatch_reply_t &','route_dispatch_reply_t &&')
 s=s.replace('submit_reply (response)','submit_reply (std::move (response))')
 s=s.replace('reply.parts.items ()','std::move (reply.parts).take_items ()')
 p.write_text(s)
for name in ['stateful/public_host_runtime.hpp','stateful/public_host_runtime.cpp']:
 p=r/name;s=p.read_text()
 # Keep the borrowed form for callers that retain a reply; add a consuming overload.
 for match in list(re.finditer(r'(?:bool public_host_runtime_t::reply|bool reply|zlink::submit_result_t reply) \(const reply_token_t &token,\s+const std::vector<zlink::message_t> &parts\)',s))[::-1]:
    sig=match.group();end=match.end()
    if name.endswith('hpp'):
        s=s[:end+1]+'\n'+sig.replace('const std::vector<zlink::message_t> &parts','std::vector<zlink::message_t> &&parts')+';'+s[end+1:]
    else:
        brace=s.index('{',end);level=1;stop=brace+1
        while level:
            if s[stop]=='{':level+=1
            if s[stop]=='}':level-=1
            stop+=1
        body=s[brace:stop].replace('encode_application (parts)','encode_application (std::move (parts))').replace('host->reply (token, parts)','host->reply (token, std::move (parts))')
        s=s[:stop]+'\n\n'+sig.replace('const std::vector<zlink::message_t> &parts','std::vector<zlink::message_t> &&parts')+'\n'+body+s[stop:]
 p.write_text(s)
