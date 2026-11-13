#!/usr/bin/env python3
import json,pathlib
repo=pathlib.Path('/root/worktree/zlink-v21-s033/l3868')
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868')
cpp='framework/languages/cpp/framework/'
bench='framework/bench/grpc/cpp/'
owner=cpp+'src/runtime/mesh/raw_mesh_node_owner.cpp'
host=cpp+'src/runtime/mesh/mesh_node_host_service.cpp'
channel=cpp+'src/runtime/channels/channel_runtime.cpp'
mesh=cpp+'include/zlink/framework/contracts/configuration/mesh_node.hpp'
def site(path,needle,meaning):
    rows=(repo/path).read_text().splitlines()
    found=[i+1 for i,line in enumerate(rows) if needle in line]
    assert found,(path,needle)
    return {'file':path,'line':found[0],'all_lines':found,'meaning':meaning}
features={
 'L1 permit':[
  site(host,'supply.ensure_waiter (dispatch_limits::receive_batch_messages)', '수신 전 application permit budget 확보'),
  site(cpp+'src/runtime/dispatch/application_job_queue.hpp','_permit.release_for_handler_entry ();','handler 첫 instruction 전 permit 반환'),
  site(owner,'raw_mesh_node_owner_t::ablation_receive (','L0 owner가 permit 없이 native recv를 drain한다')],
 'L2 envelope header':[
  site(channel,'auto header = codec.create_envelope (runtime::messaging::message_kind_t::command','source envelope 생성'),
  site(cpp+'src/runtime/channels/route_packet_dispatcher.cpp','auto header = codec.decode_header (received.parts,','target envelope 해석'),
  site(bench+'ladder/ladder_wire.hpp','return {message (request_envelope ()), std::move (body)};','L0는 고정 envelope bytes를 복사한다; encode/parse 없음')],
 'L3 service wire framing':[
  site(owner,'protocol::encode_node_send_header_message (),','source nodeSend service header'),
  site(owner,'return protocol::encode_node_request_header_message (request_correlation);','source nodeRequest service header'),
  site(owner,'const auto header = received->service_header ();','target service header와 application framing 해석'),
  site(bench+'ladder/ladder_wire.hpp','// Canonical peers retain their existing wire contract.','canonical peer와 연결할 때만 사용하는 framing 호환 처리; L0 기본 경로는 기존 raw 2-part wire')],
 'L4 codec':[
  site(cpp+'include/zlink/framework/contracts/channels/channel.hpp','auto encode_payload = [&message_value] (serializer_registry_t &serializers)','source typed serializer 호출'),
  site(mesh,'auto payload = codecs->deserialize (serializers, message,','target typed payload decode'),
  site(mesh,'detail::encoded_payload_to_raw (plan->serialize (serializers, reply))','target typed reply encode'),
  site(bench+'ladder/ladder_wire.hpp',': message (encode_bench_payload (value));','L0 source는 binding과 같은 BenchPayload 할당과 protobuf encode를 유지한다; Framework serializer를 제외한다'),
  site(bench+'ladder/ladder_wire.hpp','} else if (!payload.ParseFromArray','L0 target은 binding과 같은 protobuf decode를 유지한다; Framework codec registry를 제외한다'),
  site(bench+'ladder/ladder_wire.hpp','inline void raw_reply','L0 reply는 binding과 같은 새 envelope·body·protobuf encode를 유지한다'),
  site(bench+'ladder/ladder_driver.inc','if (!payload.ParseFromArray (reply[1].data ()','L0 reply 수신도 binding과 같은 protobuf decode를 유지한다')],
 'L5 owner queue / worker':[
  site(owner,'return _lane.run_sync ([this, parts, trace','source owner turn 진입'),
  site(owner,'raw_mesh_node_owner_t::enqueue_received (','target mailbox enqueue'),
  site(host,'void mesh_node_host_service_t::dispatch_application (','target worker dispatch'),
  site(owner,'void raw_mesh_node_owner_t::ablation_receive (','실제 owner가 자체 poller에서 block/drain하고 고정 handler를 직접 호출한다'),
  site(owner,'zlink::send_submission_t raw_mesh_node_owner_t::ablation_send (','실제 owner의 socket에 직접 submit; lane과 Framework socket mutex 없음')],
 'L6 activation / DI scope':[
  site(cpp+'src/runtime/channels/route_handler_invoker.cpp','service_scope_t::create (services, service_scope_kind_t::handler_invocation)','메시지별 invocation scope 생성'),
  site(mesh,'auto &owner = detail::create_dispatch_instance<THandler> (services);','메시지별 handler 활성화'),
  site(bench+'ladder/ladder_wire.hpp','// L6: one handler, constructed outside the receive loop, called directly.','고정 벤치 handler 하나를 직접 호출한다; handler runtime 모사가 아님')],
 'L7 registry / completion / deadline':[
  site(owner,'value.registered = _operations->register_operation (','Framework request/send operation 등록'),
  site(cpp+'src/runtime/dispatch/provider_task_context.cpp','void dispatch_completion (','Framework completion dispatcher'),
  site(cpp+'src/runtime/foundation/operation_registry.cpp','void operation_registry_state_t::reserve_deadline_visit (','Framework deadline 등록'),
  site(owner,'return std::move (operation).async ();','L0 source는 binding submission을 그대로 반환한다'),
  site(bench+'ladder/ladder_driver.inc','observe_binding (std::move (submission.reply),','source reply도 binding completion에서 벤치 stream을 완료한다')]
}
features['L5 owner queue / worker'] += [
 site(cpp+'src/runtime/backend/raw_route_port.cpp','std::lock_guard lock (*_socket_mutex);','Framework socket submission/receive gate도 L0에서 건너뛴다'),
 site(owner,'const auto admitted = _topology.peer ({source_rid.data (), source_rid.size ()});','기본 L0 data path는 logical peer 검사를 건너뛴다; canonical cross peer control은 별도 adapter'),
 site(owner,'admitted->liveness->record_received (now);','기본 L0 data path는 메시지별 logical liveness 기록을 건너뛴다')]
features['L6 activation / DI scope'] += [
 site(cpp+'src/runtime/channels/route_packet_dispatcher.cpp','? _handlers->resolve (_router_channel_id, runtime::messaging::message_kind_t::command,','고정 handler는 메시지별 handler 조회를 건너뛴다'),
 site(cpp+'src/runtime/channels/route_handler_invoker.cpp','auto pending = filters.invoke_filters_async (','handler filter/invoker와 invocation scope를 함께 건너뛴다')]
features['L7 registry / completion / deadline'] += [
 site(cpp+'include/zlink/framework/contracts/channels/call.hpp','bool try_claim () noexcept','native owner entry는 Framework public-call object와 중복 제출 claim을 건너뛴다; binding 수락과 벤치 admission sequencing은 유지')]
features['L2 envelope header'] += [
 site(channel,'auto submit_flow = runtime::flow_context_t::enter_current_or_create (','source flow context와 envelope metadata 생성 경로도 L0에서 제외한다'),
 site(cpp+'src/runtime/messaging/client_call_codec.cpp','envelope_header_t client_call_codec_t::create_envelope','source direct-node envelope의 실제 생성자; 고정 bytes로 대체')]
features['L5 owner queue / worker'] += [
 site(channel,'const auto transport = detail::resolve_node_transport (','source mesh transport 조회를 고정 실제 owner로 대체한다'),
 site(cpp+'src/runtime/mesh/mesh_node_runtime.cpp','mesh_node_runtime_t::classify_node_target_from_topology','source logical target 분류를 제외한다; binding 물리 route 선택은 유지한다'),
 site(host,'struct mesh_node_host_service_t::node_receive_role_t final','host의 수신 회전·관리/control 배치를 실행하지 않는다; L0 고정 단일 peer와 별도 admission adapter만 사용한다')]
features['L6 activation / DI scope'] += [
 site(bench+'client/bench_cpp_client.cpp','class framework_driver_t final','host/DI 구성과 route_client 획득을 실제 owner 직접 구성으로 대체한다(startup 비용)'),
 site(bench+'framework/bench_framework_cpp_server.cpp','bench_metric_reader_t metric_reader;','host/DI handler 등록과 Framework 누적 metric 수집을 제외한다; bench counters는 유지한다')]
features['L7 registry / completion / deadline'] += [
 site(channel,'detail::route_client_state_t::admit_outbound (_state);','public call의 Framework lifecycle admission을 제외한다; binding admission은 유지한다'),
 site(channel,'return detail::project_task_terminal<void> (submitted, finish);','nested Framework task terminal projection과 오류 매핑을 binding observer로 대체한다')]
(job/'raw/disabled-code-owner.json').write_text(json.dumps(features,ensure_ascii=False,indent=2)+'\n')
for feature,sites in features.items():
 print(feature)
 for s in sites: print(f"  {s['file']}:{s['line']} — {s['meaning']}")
