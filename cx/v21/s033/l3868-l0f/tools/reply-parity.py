#!/usr/bin/env python3
"""Verify source/receive/reply parity and record the exact pre-measurement excerpts."""
import pathlib, subprocess, sys
repo=pathlib.Path('/root/worktree/zlink-v21-s033/l3868')
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-l0f')
binding=repo/'framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp'
l0=repo/'framework/bench/grpc/cpp/ladder/ladder_wire.hpp'
owner=repo/'framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp'
client=repo/'framework/bench/grpc/cpp/client/bench_cpp_client.cpp'
driver=repo/'framework/bench/grpc/cpp/ladder/ladder_driver.inc'
def excerpt(path,start,end,after=None):
    rows=path.read_text().splitlines()
    offset=next(i for i,r in enumerate(rows) if after in r) if after else 0
    a=next(i for i in range(offset,len(rows)) if start in rows[i])
    b=next(i for i in range(a,len(rows)) if end in rows[i])
    return [(i+1,rows[i].strip()) for i in range(a,b+1)]
def normalized(rows):
    return [r.replace('zlink_cpp_bench::','').replace('zlink::framework::bench::withgrpc::BenchPayload','payload_t').replace('_router->','router.').replace('g_stop','stop') for _,r in rows]
def side(title,p,a,q,b):
    lines=[f'\n{title}\n',f'| binding `{p.relative_to(repo)}` | L0 `{q.relative_to(repo)}` |','|---|---|']
    for i in range(max(len(a),len(b))):
        def cell(rows):
            if i>=len(rows):return ''
            n,t=rows[i];return f'`{n}: {t}`'
        lines.append(f'| {cell(a)} | {cell(b)} |')
    return lines
lines=[]
assert 'std::optional<std::chrono::milliseconds> receive_timeout;' in owner.with_suffix('.hpp').read_text()
assert 'receive_timeout' not in '\n'.join(t for n,t in excerpt(l0, 'inline fw::runtime::mesh::raw_mesh_node_options_t owner_options (', '    return options;'))
assert 'recv_timeout' not in binding.read_text()
lines.append('RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.\n')
a=excerpt(binding,'zlink::received_t received;', '        try {',after='void request_loop')[:-1]
b=excerpt(owner,'zlink::received_t received;', '        handler (state, received);',after='void raw_mesh_node_owner_t::ablation_receive')[:-1]
assert normalized(a)==normalized(b),(a,b)
a_cmd=excerpt(binding,'zlink::received_t received;', '        try {',after='void command_loop')[:-1]
assert normalized(a_cmd)==normalized(b)
assert 'dontwait' not in '\n'.join(t for n,t in b) and '->wait' not in '\n'.join(t for n,t in b)
lines+=side('수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.',binding,a,owner,b)
a=excerpt(binding,'const char *envelope =','encoded.data (), encoded.size ())))')
b=excerpt(l0,'const char *envelope =','encoded.data (), encoded.size ())))')
assert normalized(a)==normalized(b),(a,b)
lines+=side('응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.',binding,a,l0,b)
submit='received.reply ().message (header).message (echo).submit ();'
assert submit in binding.read_text() and submit in l0.read_text()
for left,right in [('payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))','payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))'),('metrics.record (payload.body ().data (), payload.body ().size ());','metrics.record (payload.body ().data (), payload.body ().size ());'),(submit,submit)]:
    a=excerpt(binding,left,left);b=excerpt(l0,right,right);lines+=side('수신 decode·counter·reply submit 대조',binding,a,l0,b)
# Verify the shared submit body was moved verbatim from the original raw client.
old=subprocess.check_output(['git','-C',str(repo),'show','666edd8dc2:framework/bench/grpc/cpp/client/bench_cpp_client.cpp'],text=True)
start=old.index('    void submit (std::shared_ptr<stream_t> stream, int, uint64_t seq) override',old.index('class zlink_raw_driver_t'))
end=old.index('    zlink::context_t _context;',start)
old_body=old[start:end];old_body=old_body[old_body.index('    {'):].replace('_socket.', 'socket.').replace('(_target)', '(target)')
current=client.read_text();start=current.index('    void submit_raw (');start=current.index('    {',start);end=current.index('    virtual void prepare_streams',start)
assert current[start:end]==old_body,'shared raw submit differs from original binding code'
assert 'submit_raw (_socket, _target, std::move (stream), seq);' in current
assert 'submit_raw (*_socket, _target, std::move (stream), seq);' in driver.read_text()
lines+=side('source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.',client,excerpt(client,'submit_raw (_socket','submit_raw (_socket'),driver,excerpt(driver,'if (_level == 0) {','        auto parts = _encoders[index]->encode')[:-1])
lines+=['',f'공유 source 구현 `{client.relative_to(repo)}`:', '```cpp']+[f'{n}: {t}' for n,t in excerpt(client,'const char *envelope =','if (_command)',after='void submit_raw')]+['```']
a=excerpt(client,'zlink::poll_event_t event{};','        });',after='class zlink_raw_driver_t')[:-1]
b=excerpt(driver,'zlink::poll_event_t event{};','                } else {',after='if (_owner) {')[:-1]
assert [t.replace('_poller.wait','poller.wait') for n,t in a]==[t.replace('_owner->ablation_poller ().wait','poller.wait') for n,t in b],(a,b)
lines+=side('source 완료 대기 일치: POLLCOMPLETION poller.wait(50ms), recv probe 없음.',client,a,driver,b)
lines+=['''
| record당 작업 · 정상 경로 | binding | L0 |
|---|---|---|
| target wait | blocking recv 1; 별도 poll 0 | 동일 |
| source 완료 wait | POLLCOMPLETION 50ms wait; binding이 callback drain | 동일 |
| source payload 생성 | BenchPayload body 문자열 객체·buffer, protobuf encoded 문자열: 생성 3; Message buffer 2; protobuf encode 1 | 같은 submit_raw; 추가 생성 0 |
| source 명시 복사·쓰기 | body assign 1, stamp 1, protobuf encode write 1, header/body Message::from memcpy 2 | 동일; 빈 Message init·array move 없음 |
| target protobuf decode | body 문자열 객체·buffer 생성 2, decode copy 1 | 동일 |
| request reply 생성 | reply body 객체·buffer 생성 2, encoded 문자열 1, envelope/body Message buffer 2; encode 1 | 동일 |
| request reply 복사 | reply.set_body 1, protobuf encode write 1, Message::from memcpy 2 | 동일; request part 재사용 0 |
| target metrics | server_metrics_t::record 1; mutex 1; counter atomic 0; latency vector의 공통 성장 비용 | 같은 class·호출 1 |
| target stop atomic | stop.load 1/record | 동일 |
| source counters·callback | driver_t::run/stream_t와 observe_binding 공통 구현; sequence.fetch_add 1; callback의 공통 refcount·counter atomic | 동일; L0 추가 atomic 0 |
| binding/Core | 동일한 router send/request/reply·native Message alloc·completion; native encoder buffer copy | 동일 코드·동일 library; 위 표는 binding 내부의 조건부 allocation·atomic을 별도 총계로 추정하지 않음 |

생성 수는 bench가 요청하는 객체·buffer의 수다. allocator cache·protobuf 내부의 세부 malloc 횟수와 binding의 조건부 완료 상태 생성은 총계로 추정하지 않는다. 양쪽이 같은 구현을 호출한다. 근거: message.cpp:108–115, bench_common.hpp:159–165, bench_stats_server.hpp:45–63. L0 fixed_handler는 직접 호출되며 legacy view의 빈 vector는 heap을 할당하지 않는다. Canonical wire adapter는 측정하는 legacy 메시지에 실행되지 않는다.
''']
text='\n'.join(lines)+'\n'
cell=pathlib.Path(sys.argv[1]);cell.mkdir(parents=True,exist_ok=True)
(cell/'reply-parity.md').write_text(text)
with (job/'progress.md').open('a') as f:f.write(text)
print('PASS: blocking receive, fresh reply, raw source payload/lifetime/submit, completion wait parity')
