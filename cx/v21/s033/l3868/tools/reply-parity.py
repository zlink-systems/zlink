#!/usr/bin/env python3
import pathlib,sys
repo=pathlib.Path('/root/worktree/zlink-v21-s033/l3868')
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868')
binding=repo/'framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp'
l0=repo/'framework/bench/grpc/cpp/ladder/ladder_wire.hpp'
def excerpt(path,start,end):
    rows=path.read_text().splitlines()
    a=next(i for i,r in enumerate(rows) if start in r)
    b=next(i for i in range(a,len(rows)) if end in rows[i])
    return [(i+1,rows[i].strip()) for i in range(a,b+1)]
a=excerpt(binding,'const char *envelope =','encoded.data (), encoded.size ())))')
b=excerpt(l0,'const char *envelope =','encoded.data (), encoded.size ())))')
def normalized(rows):
    return [r.replace('zlink_cpp_bench::','').replace('zlink::framework::bench::withgrpc::BenchPayload','payload_t') for _,r in rows]
assert normalized(a)==normalized(b),(a,b)
submit='received.reply ().message (header).message (echo).submit ();'
assert submit in binding.read_text() and submit in l0.read_text()
assert 'payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))' in l0.read_text()
assert 'payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))' in binding.read_text()
lines=['\nBinding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.','',f'| binding `{binding}` | L0 `{l0}` |','|---|---|']
lines += [f'| `{x}: {t}` | `{y}: {u}` |' for (x,t),(y,u) in zip(a,b)]
lines += ['| `'+str(next(n for n,r in enumerate(binding.read_text().splitlines(),1) if submit in r))+': '+submit+'` | `'+str(next(n for n,r in enumerate(l0.read_text().splitlines(),1) if submit in r))+': '+submit+'` |']
for left,right in [('payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))','payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))')]:
    x=next(n for n,r in enumerate(binding.read_text().splitlines(),1) if left in r)
    y=next(n for n,r in enumerate(l0.read_text().splitlines(),1) if right in r)
    lines.append(f'| `{x}: {left}` | `{y}: {right}` |')

text='\n'.join(lines)+'\n'
(pathlib.Path(sys.argv[1])/'reply-parity.md').write_text(text)
with (job/'progress.md').open('a') as f:f.write(text)
print('binding/L0 fresh reply allocation/copy/encode/submit statements match')
