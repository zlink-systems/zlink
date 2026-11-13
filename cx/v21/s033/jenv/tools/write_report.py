from pathlib import Path
import json,sys
import report_tables as T
P=T.P;W=Path('/root/worktree/zlink-jenv')
cases=('main','prototype-unpaced','prototype','binding')
extra={c:T.load(c,'extra-summary.json') for c in cases}
result={c:T.load(c,'results.json')['cells'][0] for c in cases}
for c in cases:
 r=result[c]
 assert r['submitted']==r['completed']==r['target_stats']['received']
 assert r['errors']==r['abandoned']==r['target_stats']['errors']==0
 records=[h['part_sizes'] for stream in T.load(c,'wire.json') for h in stream['record_hist'] if sum(h['part_sizes'])>1000]
 expected=([13,1398],[21,1398]) if c=='main' else (([201,1027],) if c=='binding' else ([41,1027],))
 assert all(x in expected for x in records),(c,records)
mem=min(T.load(c,'runner-meta.json')['min_memavailable_kb'] for c in cases)/1048576
assert mem>=12
text=T.first()
text+=f'''단위는 완료 요청 하나당 CPU 시간이다. 첫 표의 prototype과 binding은 main의 전체 처리율을 반올림한 {round(extra['main']['throughput']):,} req/s로 pacing했다. Source는 CPU 0–7, target은 8–15에 고정한 condition B다. request-backpressure, payload 1,024 B, warmup 30초, active 60초, 최종 유효 실행은 조건마다 1회다. 각 실행은 `/root/jobs/fx/cx/measure.lock`의 배타 lock 안에서 진행했다. MemAvailable 최솟값은 {mem:.2f} GiB다.

Prototype unpaced 처리율은 {extra['prototype-unpaced']['throughput']:,.3f} req/s이고 caller는 {extra['prototype-unpaced']['caller_us']:.3f} µs다. Source/target mesh CPU는 {sum(v['cpu_us'] for v in extra['prototype-unpaced']['mesh']['source']):.3f}/{sum(v['cpu_us'] for v in extra['prototype-unpaced']['mesh']['target']):.3f} µs/request다. Main 대비 처리율은 {extra['prototype-unpaced']['throughput']/extra['main']['throughput']:.3f}배다. 각 조건의 완료 수는 main {result['main']['completed']:,}, prototype unpaced {result['prototype-unpaced']['completed']:,}, prototype paced {result['prototype']['completed']:,}, binding paced {result['binding']['completed']:,}건이다. 모두 submitted = completed = target received이고 source errors, target errors, abandoned는 0이다.

첫 표의 CPU는 active 안의 약 10초 schedstat delta를 같은 창의 완료 수로 나눈 값이다. 전체 처리율과 perf/BPF 창 처리율을 별도로 적었다. 평균 pacing이 같아도 수집 창과 도착 간격은 완전히 같지 않으므로 작은 차이는 정밀한 인과 효과로 해석하지 않는다. Caller는 각 `results.json`의 `jvm_thread_cores × 1e6 / throughput_per_second`다.

201 B는 비교 bench가 만드는 JSON application part다. 생성 위치는 `framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/RawWire.java:23`의 `REQUEST_ENVELOPE`와 `:29`의 `RESPONSE_ENVELOPE`다. Request의 kind만 1이고 reply는 2다. `channelName=bench`, `messageName=BenchPayload`, `contentType=application/x-protobuf`이며 correlationId, deadline, topic, errorCode, errorMessage, source는 null이다. Capture에서도 두 방향의 native parts가 `[201, 1027] B`이고 framing 24 B를 더한 record가 1,252 B였다.

이 bench는 `bindings/java/perf` harness를 사용하지 않는다. `RawStack.java:69–77`에서 public `RouterSocket.request(peer).message(header).message(body).timeout(timeout).submit()`을 직접 호출한다. Server는 `ZLinkRawBenchServer.java:115–123`에서 `RESPONSE_ENVELOPE`를 만들고 원래 `received.reply()`에 두 part를 전달한다. Binding의 `NativeRouterSocket.java:82`는 받은 parts를 request builder에 전달하며, `CompletionOwner.java:605–654`는 native completion token으로 요청을 찾는다. 201 B JSON을 자동으로 삽입하는 코드가 아니다. 실제 dependency는 Java binding 1.23.0 package다.

Native part와 record 구성은 다음과 같다. Application payload 1,027 B는 세 조건에서 같은 protobuf BenchPayload다. 측정용 1,024 B body와 protobuf tag/length 3 B를 포함한다.

'''
text+=T.table(['실행','request native parts B','reply native parts B','추가 구성'],[
 ['main','13 + 1398','21 + 1398','1398 = wrapper 75 + JSON header 296 + payload 1027'],
 ['prototype','41 + 1027','41 + 1027','descriptor 41, wrapper와 JSON header 없음'],
 ['binding','201 + 1027','201 + 1027','bench JSON header 201'],
])
decoded=json.load(open(P/'decoded-examples.json'))
text+='각 capture에서 request와 reply 하나씩 해석한 결과는 다음과 같다. 두 Framework 조건은 같은 요청의 identity가 reply에 보존됐다. 상세 header와 hex는 `decoded-examples.json`과 각 `wire.json`에 있다.\n\n'
rows=[]
for c in ('main','prototype','binding'):
 for r in decoded[c]:
  h=r['header']
  rows.append([T.labels[c],r['direction'],h.get('operation',h.get('kind')),h.get('channel',h.get('channelName')),h.get('message',h.get('messageName')),h.get('codec',h.get('contentType')),h.get('identity128',h.get('correlationId')) or 'null',h.get('deadline') or '없음'])
text+=T.table(['실행','방향','operation 또는 kind','channel','message','codec 또는 content type','identity / correlationId','deadline'],rows)
text+='''각 방향의 ZMP framing은 모두 24 B이고 native part 수는 모두 2개다. Prototype descriptor는 magic 2 B, major 1 B, operation 1 B, flags 1 B, codec 1 B, identity 16 B, channel text8 6 B, message text8 13 B다. Node request command는 생성 상수와 실제 capture에서 17이고 Channel request는 19, reply는 20이다. Request 종류를 별도 kind byte로 반복하지 않는다. 이 bench는 deadline을 wire에 넣지 않으며 binding timeout 30초를 유지한다. Descriptor의 선택 deadline 표현은 epoch milliseconds 8 B다. 이 프로토타입은 빈 metadata와 flow off만 허용하며 해당 필드를 생략한다. 일반 Node/Channel request는 major 2만 받으며 JSON 해석 실패 후 다른 형식을 시도하는 fallback은 없다. Control traffic의 기존 major 1 처리는 유지한다.

높음으로 분류했던 TCP 가설과 socket 옵션은 아래 계측으로 판단한다.

'''
text+=(P/'cause-analysis.md').read_text()+'\n\n'
text+='''아래 I/O 분해는 cpu-clock 49 Hz 표본의 stack 분류 비율을 각 thread의 schedstat CPU에 적용한 추정이다. TCP send/receive는 sendto/recvfrom과 그 하위 stack을 포함한다. Epoll은 wait/ctl, eventfd/wake는 read/write 경로다. Core userspace는 syscall 범주 밖의 I/O thread 표본이며 allocator와 memcpy도 포함한다. 기타 syscall은 별도 열에 남겼다. 한 표본을 두 범주에 넣지 않았다. CPU delta가 있으나 cpu-clock 표본이 없는 idle I/O thread는 미표본 열에 남겼다.

'''+T.stages()
text+="기본 실행의 BPF 계수는 다음과 같다. 계수 창 길이는 counter_stats의 전·후 통계 조회 블록 중간 시각 차이를 썼다. Process snapshot에 걸린 시간은 창 길이에 넣지 않았다. 성공 호출의 log2 byte histogram을 합친 분포다. HTTP 관리 socket은 제외했다.\n\n"+T.syscalls()
text+="""전체 process BPF 추적은 binding의 계수 창을 약 52.2k req/s로 낮췄다. I/O tid의 TCP syscall만 추적한 61,132 req/s 재계수에서는 prototype/binding이 약 61k였으나 main은 약 56.5k로 내려갔다. 이 차이를 동일 rate 비교로 간주하지 않았다. TCP 원인 확인용으로 세 조건 모두 여유가 있는 50,000 req/s에서 warmup 30초, active 60초, 각각 1회 추가 계측했다. 아래 50k 표에는 해당 실행의 전체·perf·BPF rate를 모두 적었다.

추가 계측의 main은 보존한 main runtime과 target을 그대로 쓰고 source의 client.jar만 Framework pacing이 추가된 bench driver로 바꾼 별도 main-counter-build 사본을 사용했다. 원래 main-build 사본과 첫 표의 기본 성능 측정은 바꾸지 않았다. 50k 확인에서는 JFR을 켜지 않고 I/O/mesh thread 대상 perf와 I/O tid 대상 TCP BPF를 썼다. Framework/runtime·binding/native library는 기본 실행과 같다.

"""+T.cause_compare()+T.syscalls(focused=True)
text+='''Socket 옵션은 해당 프로세스의 tcp_sendmsg/tcp_recvmsg 진입에서 실제 kernel sock을 읽었다. SO_SNDBUF/SO_RCVBUF는 kernel이 보유하는 값이며 userspace가 설정한 원래 인자와 구분한다. 수신 buffer는 관찰 구간에 자동 조정될 수 있어 범위를 적었다. 기본 61k 실행과 50k TCP 확인 실행을 구분해 두 값을 모두 보존했다. sk_userlocks도 함께 기록했다.

'''+T.sockets()
text+='''기본 61k 실행의 kernel TCP cycles/B는 thread별 10초 cycles:k hardware counter에 TCP/IP/socket 하위 stack의 cycle sample period 비율을 적용하고, 같은 실행의 완료 rate × 10초 × 해당 방향의 capture record B로 나눈 추정이다. Perf timestamp와 Python monotonic의 clock 원점이 달라 절대 timestamp 교집합으로 표본을 거르지 않았다. Kernel 밖의 memcpy는 이 값에 포함하지 않았다. 같은 thread의 cycles:k counter도 `capture/*.cycles.csv`에 보존했다. Sampling과 수집 경계의 in-flight 차이가 있으므로 소수점 이하까지 정확한 hardware 계수로 해석하지 않는다.

'''+T.kernel()
text+='''Receive allocator 계측은 로드한 libzlink.so의 `zmp_decoder_t::size_ready` 진입·반환 uprobe를 사용했다. 실제 라이브러리의 disassembly로 allocator 범위와 storage type 위치를 확인했다. Part 전체가 allocator 범위 안에 들어가는지와 성공 반환 후 storage type을 셌다. 아래는 probe가 활성화된 약 5초의 비율이며, schedstat snapshot 약 3초와 나눠 per-request 비용을 계산하지 않았다. Type 101은 inline storage, 105는 reference-counted storage다. 105라는 사실만으로 allocator 범위 안의 zero-copy 성공이라고 세지 않았다.

'''+T.decoder()
text+='''Mesh source의 JFR은 profile 설정으로 active 60초를 기록했다. 기존 jio와 같이 stack에서 먼저 만나는 registry take, reply header decode, multipart view decode, completion handoff, other에 한 번만 배정했다. 같은 실행의 mesh schedstat CPU에 표본 비율을 적용했다. Binding에는 Framework mesh thread가 없다. Prototype의 새 descriptor 해석은 other에 포함한다.

'''+T.jfr()
text+='Main의 registry take·reply decode·multipart decode 합계는 2.813 µs지만 mesh CPU의 실제 감소는 1.324 µs다. Completion handoff와 other가 늘었으므로 제거한 단계의 합계를 순 절감으로 보고하지 않는다.\n\n'
text+='''직접 변경한 worktree 파일은 다음과 같다. 기존 CPU-pin 패치와 raw Java pacing 옵션은 유지했다. BenchOptions.java와 다른 언어 runner의 기존 변경은 이번 작업에서 만들지 않았다.

'''
base='framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/'
text+=T.table(['파일','변경 이유'],[
 [base+'messaging/ZLinkChannelEnvelope.java','기존 Header 타입에 compact descriptor encode/decode를 구현한다.'],
 [base+'channels/ZLinkChannelCallRuntime.java','Node/mesh Channel의 descriptor 생성과 reply 해석을 명시적으로 연결한다. ClientServer 해석 경로는 기존 호출로 구분한다.'],
 [base+'channels/ZLinkChannelRouteCalls.java','Node와 mesh Channel 요청이 native descriptor와 typed payload를 만들도록 연결한다.'],
 [base+'binding/ZLinkJavaRawMeshNode.java','Public binding request completion으로 결과를 받고 원래 reply token으로 native parts를 회신한다. 일반 hop의 registry 등록과 service correlation echo를 제거한다.'],
 [base+'channels/ZLinkMeshApplicationDispatcher.java','Native descriptor로 typed dispatch하고 성공 reply descriptor를 만든다.'],
 [base+'internal/service/ZLinkServiceOperationRegistry.java','기존 prepared Operation의 typed receiver와 observer를 binding terminal에서 직접 완료한다. Registry entry를 추가하지 않는다.'],
 ['framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java','기존 request rate pacing을 Framework backpressure loop에도 적용한다.'],
])
text+='''일반 요청 경로에서 service request/reply header, 75 B multipart wrapper, packed part 수와 길이 해석, JSON header encode/decode, hop matching용 `submitTerminal` 등록과 `take` 조회를 제거했다. 호출이 없어진 request overload 2개와 decodeRequestReply helper도 삭제했다. Typed payload serializer와 typed handler dispatch는 유지한다. 추가한 것은 descriptor codec, 명시적인 Node/Channel transport 경로 선택, 기존 Operation의 direct binding completion 진입점, bench pacing 연결이다. Durable request와 다른 경로의 registry 구현은 남겨 뒀다.

리팩토링 점검은 중복 service header, wrapper, JSON header, hop registry의 4항목을 확인하고 이 경로에서 제거·대체했다. 이 변경으로 호출이 없어진 helper 3개도 삭제했다. 범위 밖으로 이관한 항목은 0이다. 새 queue, lock, cache, persistent state는 0이다. 일반 hop의 요청·응답 매칭 판정 위치는 binding matching + service correlation 검사 + Framework registry 조회의 3곳에서 binding matching 1곳으로 줄었다. 기존 reply winner와 execution gate를 재사용했다.

Main은 소스 수정 전에 `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh`로 빌드하고 runnable install output을 `main-build/`에 복사했다. 기준 commit은 사용자가 지정한 213e579f52다. Git metadata 조회는 runner의 stub으로 막았으므로 results.json의 commit은 unknown이다. Prototype도 같은 명령으로 빌드했다. 필요한 Framework Java composite build가 모두 같은 wrapper 안에서 수행됐다. 최종 descriptor 검사는 41 B와 payload 1,027 B, 128-bit identity 전체 보존을 확인했고 잘린 입력 41개, 이전 major 1개, 잘못된 operation 1개를 거부했다. 정식 cross-language/Spot/Actor/ClientServer 호환성, metadata/flow on, 오류·취소·deadline 일반 계약은 이 실험의 완료 조건에 포함하지 않았다.

소유 계층은 Core/binding의 transport matching, reply token, completion, multipart와 Framework의 논리 대상, typed dispatch, end-to-end identity, flow, metadata, deadline으로 나눴다.

기준 조항은 design.md 5절, binding async-execution-model 4·5절, wire protocol 2·4·11절, flow-correlation 2·3·5절이다. 새 descriptor는 사용자 승인 범위의 측정용 형식이며 정식 스펙은 수정하지 않았다.

교차언어 대조는 제공된 design.md의 C++/.NET/JVM/Node 현행 wire 대조를 기준으로 했다. 다른 언어 구현이나 parity test는 변경·실행하지 않았다.

변경 분류는 D에 해당하는 새 application wire 실험이다. 사용자에게 승인받은 Java 측정용 프로토타입이며 배포·commit 대상이 아니다.

재현 자료는 `runs/<조건>/results.json`, `wire.pcap`, `wire.json`, `capture/`, `capture-bpf/`, `capture-decoder/`, `source.jfr`에 있다. `main-build.log`, `prototype-build.log`, `descriptor-check.log`, `build-manifest.json`과 `prototype.diff`를 보존했다. 앞선 all-thread profiler 진단 run과 중단된 수집기·smoke 실행은 최종 표에 합산하지 않았다. 기본 성능 조건 4개는 각 1회다. 61k I/O 재계수 3개와 50k 원인 확인 3개도 각각 1회이며 모두 요청 누락·오류·abandoned가 0이다. 남은 실행 실패는 없다. 종료에는 SIGTERM/SIGINT를 사용했고 shell background `&` 실행은 사용하지 않았다.
'''
(P/'report.md').write_text(text)
print('report.md',len(text),'characters')
