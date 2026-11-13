from pathlib import Path
root=Path('framework/languages/cpp/framework/src/runtime')
for name in ('mesh/raw_mesh_node_owner.cpp','mesh/raw_mesh_node_owner.hpp','protocol/service_wire_codec.cpp'):
 p=root/name;s=p.read_text()
 import re
 def resolve(m):
  a,b=m.group(1),m.group(2)
  if name.endswith('service_wire_codec.cpp'): return a
  if 'start_admitted_request' in a:
   return a.replace('const std::vector<std::uint8_t> &target_routing_id','std::span<const std::uint8_t> target_routing_id').replace('std::function<void (result_t<detail::backend::raw_request_completion_t>)> completion','detail::backend::raw_request_callback_t completion')
  if 'wire;' in a: return b.replace('detail::backend::raw_message_t','std::vector<zlink::message_t>')
  return a.replace('const std::vector<std::uint8_t> &target_routing_id','std::span<const std::uint8_t> target_routing_id')
 s=re.sub(r'<<<<<<< HEAD\n(.*?)=======\n(.*?)>>>>>>> [^\n]*\n',resolve,s,flags=re.S)
 if name.endswith('.hpp'): s=s.replace('detail::backend::raw_message_t _request_wire;', 'std::vector<zlink::message_t> _request_wire;')
 p.write_text(s)
for ext in ('hpp','cpp'):
 p=root/f'backend/raw_route_port.{ext}';s=p.read_text()
 # Change only native overload and shared request_impl; send APIs stay owning.
 s=s.replace('void raw_route_port_t::request (const raw_bytes_t &target_routing_id,\n                                std::vector<zlink::message_t> parts,','void raw_route_port_t::request (std::span<const std::uint8_t> target_routing_id,\n                                std::vector<zlink::message_t> &&parts,')
 s=s.replace('void request (const raw_bytes_t &target_routing_id,\n                  std::vector<zlink::message_t> parts,','void request (std::span<const std::uint8_t> target_routing_id,\n                  std::vector<zlink::message_t> &&parts,')
 s=s.replace('std::function<void (result_t<raw_request_completion_t>)> completion','raw_request_callback_t completion')
 s=s.replace('void request_impl (const raw_bytes_t &target_routing_id,\n                       Parts parts,','void request_impl (std::span<const std::uint8_t> target_routing_id,\n                       Parts &&parts,')
 s=s.replace('void raw_route_port_t::request_impl (\n  const raw_bytes_t &target_routing_id,\n  Parts parts,','void raw_route_port_t::request_impl (\n  std::span<const std::uint8_t> target_routing_id,\n  Parts &&parts,')
 p.write_text(s)
