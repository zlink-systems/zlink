# review-SD-2 — STREAM 큰 payload·WS frame 경계 수정 차단 검증

검토 기준은 base `84d25131a6`과 고정 artifact
`/home/hep7hep7/project/zlink-work/all-artifacts/SD-2.patch`
(SHA-256 `f564368079e12a9108bfbe95e8ad9516d9e2f634cfe970fc0441007cbf395b5d`)이다.
동시에 변경 중인 `sd1` worktree는 읽지 않았다. B는 채택 차단, W는 비차단 경고·증거 한계,
S는 확인 결과·후속 제안이다. 소스·스펙·patch를 수정하지 않았고 빌드·테스트·benchmark를
실행하지 않았다. 작성한 파일은 이 보고서와 `progress-review-sd2.md`뿐이다.

## 결론

계측 app 수정, B2와 D-f는 정적으로 채택 가능하다. B1은 B2와 의미상 분리 가능하며, 실제
`SD-2-ablation-B2only.patch`도 B1 선언·상태 없이 B2를 구성한다. 그러나 WS gate 수정은 현재
형태로 채택할 수 없다.

ZMP 계약에는 “ZMP frame 하나 = WebSocket message 하나” 규칙이 없다. 오히려 binary message
경계 중간에서 ZMP frame이 계속될 수 있고 한 binary message에 여러 ZMP frame을 둘 수 있다
(`core/doc/spec/core/protocol/01-zmp.ko.md:125-127,381-387,481-492,589-597`). 따라서 기존
header/body 별도 binary message는 wire 위반이 아니다. 더 중요한 문제는 새 기본 gate가
단일 ZMP message만 꺼내는 기존 진단용 gather 경로를 모든 WS/WSS ZMP connection에 켠다는
점이다. 이 경로는 현재 준비된 여러 frame을 encoder target까지 모으는 bounded batch를
우회하고, header+body 합이 128 KiB max 이하인지도 검사하지 않는다
(`asio_engine.cpp:729-782`, `ws_batch_policy.hpp:12-23`). 이는 §9의 현재 준비된 byte bounded
batch와 “frame마다 Beast write를 시작하지 않는다”는 구현 규칙에 어긋난다. B-SD2-1이다.

## 항목별 판정

| # | 항목 | 판정 | 근거 파일:행과 판단 |
|---:|---|---|---|
| 1 | B2 1-hit 2배 성장 | **통과** | `asio_stream_fastpath_policy.hpp:338-416`의 `next_stream_target_after_full_hit()`은 한 번의 full request에서 `min(current*2,max)`를 반환한다. overflow를 피하려 `current > max/2`이면 바로 max를 택한다. decoder와 encoder 호출부는 `asio_engine.cpp:822-858`에서 같은 규칙을 쓴다. |
| 1 | B2 축소·max | **통과 / W-SD2-2** | Encoder의 message-boundary shrink 조건 `current > initial && filled < current/2 → initial`은 그대로다(`asio_stream_fastpath_policy.hpp:399-416`). Initial/max 계산과 `rcvbuf`·`sndbuf`·`maxmsgsize` clamp는 patch가 건드리지 않는다(base `asio_stream_fastpath_policy.hpp:245-348`). 다만 “connection당 1 MiB hard cap”이라는 보고서 표현은 틀리다. 1 MiB는 벤치가 준 `rcvbuf` 값(`run_benchmarks.sh:316-317`, zlink app `:42-43`)에서 decoder target max가 유도된 값이고, 런타임 기본 `rcvbuf=-1`에서는 initial cap 4 KiB가 max다(`08-stream.ko.md:389-408`). |
| 1 | `last_read_bytes` 일관성 | **통과(누적 patch) / B1 제거 대상** | Async arm은 값을 0으로 만든 뒤 성공 completion에서 실제 byte를 기록하고(`asio_engine.cpp:442-535,911-913`), speculative 경로도 시도 전에 0, 성공 뒤 실제 byte를 기록한다(`:539-596`). `last_read_request_size`도 두 경로에서 해당 요청 크기로 함께 바뀌므로 short read 뒤 과거 full 값으로 성장하는 순서는 없다. 다만 이 단일 상태와 이를 읽는 drain predicate는 B1 소유다. B1을 되돌린 채택본은 B2-only artifact처럼 기존 `last_speculative_read_bytes`와 callback 인자를 유지해야 한다. |
| 1 | 64 KiB RSS | **수치 확인 / W-SD2-2** | 보고서 `core-rf-SD-2-report.md:128-136`의 279,192→423,340 KiB는 +144,148 KiB=140.77 MiB, +51.63%다. 1,000×1 MiB보다 작다는 산술은 맞지만 decoder target 합은 process RSS hard cap이 아니다. Queue message lifetime, allocator·kernel overhead가 별도이며 Core memory spec도 budget/monitor를 process hard cap으로 보지 않는다(`05-connection-memory.ko.md:91-109,140-145`). 따라서 “상한 안”은 관측값 설명일 뿐 hard-cap 증명이 아니다. |
| 2a | WS message와 ZMP frame 대응 | **보고서 근거 기각 / W-SD2-1** | ZMP §8/§9는 WebSocket을 byte carrier로 정의하며 frame/message 경계를 독립시킨다(`01-zmp.ko.md:381-387,481-492,595-596`). `08-stream.ko.md:308-319`의 “논리 message 하나가 frame 하나”는 RAW STREAM write 설명이고 ZMP socket에 적용되지 않는다. 별도 `core/doc/spec/core/transport/*ws*` 문서는 이 tree에 없으며 대응 계약은 위 두 문서가 소유한다. |
| 2b | RAW STREAM·TCP 불변 | **통과** | 새 predicate는 `protocol_builds_gather_header && supports_gather_write && (message_boundary || ZLINK_ASIO_GATHER_WRITE)`다(`asio_stream_fastpath_policy.hpp:136-148`). RAW engine은 header를 만들지 않아 항상 false이고, TCP는 message boundary가 아니므로 ZMP도 기존 env opt-in일 때만 gather다. WS/WSS만 `has_message_boundaries()==true`이고 gather capability를 알린다(base `ws_transport.hpp:69,84-92`, `wss_transport.hpp:77,92-100`). |
| 2c | bounded batch | **차단 — B-SD2-1** | `prepare_gather_output()`은 `_next_msg`를 정확히 한 번 호출하고 header/body 한 쌍을 즉시 `async_writev()`로 제출한다(base `asio_engine.cpp:729-782`). Normal `prepare_output_buffer()`처럼 준비된 후속 frame을 현재 target까지 모으지 않으며(`:1229-1279`), 합계가 WS max 128 KiB 이하인지 확인하지도 않는다(`ws_batch_policy.hpp:12-23`). 예를 들어 encoder target이 128 KiB인 상태에서 준비된 64 KiB frame 뒤 작은 frame이 있어 둘이 target 안에 들어가도 첫 frame만 제출한다. 128 KiB보다 큰 단일 body도 그대로 한 operation에 들어간다. 이는 `01-zmp.ko.md:489-492`와 대조 보고서 `core-rf-S-D-cppserver-contrast.md:270,279-290`의 bounded 규율을 보존했다고 볼 수 없다. |
| 2c | 64 KiB+header Beast 수명·부분 write | **통과** | WS/WSS `async_writev()`는 stack의 `const_buffer` 배열을 Beast composed `async_write`에 넘긴다(base `ws_transport_common_internal.hpp:237-266`). Beast는 buffer-sequence 객체를 복사하지만 underlying memory는 completion까지 caller가 유지해야 한다(`core/external/boost/boost/beast/websocket/stream.hpp:2588-2617`). Header는 engine pipeline 배열, body는 `_pipeline.tx_msg`가 소유하고 `finish_gather_output()`이 completion에서만 닫으므로 조건을 만족한다(`asio_engine.cpp:764-799,1038-1065`). `auto_fragment(false)`이고 한 buffer sequence가 entire message payload이므로 64 KiB+header는 한 binary message/frame으로 제출된다(base `ws_transport_common_internal.hpp:107-114,252-265`). 성공 completion은 전체 composed write 뒤 오며, 오류 때의 partial byte는 connection error로 끝내므로 잘못된 tail 재제출은 없다. |
| 2d | 수신·0.17.3 혼용 | **정적 통과 / W-SD2-3** | 현재 decoder는 WebSocket FIN/message 완료로 ZMP frame을 끝내지 않고 header 길이로 복원한다(`01-zmp.ko.md:481-487`). `test_zmp_ws_wss.cpp:270-341`은 raw Beast peer가 ZMP frame을 여러 binary message로 나눈 경우와 한 message에 두 frame을 넣은 경우를 모두 검증한다. `core/v0.17.3`은 base의 ancestor이며 그 tag의 `asio_engine.cpp:403-413,845-910`과 WS `async_read_some` 경로도 같은 byte-carrier decode를 사용한다. 그러므로 header+body 결합/분리 양쪽 wire는 0.17.3과 호환된다. 단, 보고서에는 실제 mixed-version/제3 구현 송수신 실행은 없다. |
| 3 | D-f 접근자·기존 env | **통과** | base에서 세 이름의 production 사용은 `asio_stream_fastpath_policy.hpp:29-42,161-162`뿐이고 patch가 accessor와 마지막 사용을 함께 제거한다. `ZLINK_ASIO_GATHER_WRITE` accessor는 `:14-17`에 남고 TCP/non-boundary opt-in 의미도 유지된다. |
| 3 | 08-stream 삭제 초안 | **통과** | base `08-stream.ko.md:405-407`은 세 변수가 “호환을 위해 읽기만 한다”고 서술한다. 접근자 제거 뒤에는 bullet 전체 삭제가 정확하다. RAW STREAM은 계속 gather하지 않으므로 ZMP WS 자동 gather를 STREAM 기본값 항목으로 대신 넣을 필요는 없다. 영문 `08-stream.en.md:433-436`도 동시 삭제 대상이다. |
| 4 | 계측 app 조립 | **통과** | patch는 chunk-local parser를 제거해 모든 non-exact chunk를 RID별 `frame_buffer_t`에 append한 뒤 가능한 frame을 순서대로 consume한다(채택본 `test_scenario_stream_zlink.cpp:207-255`; helper `stream_echo_common.hpp:82-193`). disconnect 때 해당 RID state를 지우고, 다른 pull stack의 connection별 누적 규칙과 같다. |
| 4 | exact 1-frame zero-copy | **통과** | buffer가 비어 있고 `payload_size == 6+header+body`, size/name 검증까지 성공한 정확히 한 frame만 원본 `zlink_msg_t`를 `zlink_send_part_rid()`에 넘긴다(채택본 `test_scenario_stream_zlink.cpp:87-104,232-247`). 다중 frame, prefix/suffix, 이전 partial이 있으면 모두 조립·복사 경로다. |
| 5 | B1 분리 가능성 | **통과(의미상), 기계적 선택 적용 필요** | `SD-2-ablation-B2only.patch`는 B1의 readiness virtual/TCP override, `last_read_bytes`, drain/restart 재작성 없이 app+B2+D-f를 이미 구성한다. WS 수정이 추가로 요구하는 production 선언은 기존 `i_asio_transport::has_message_boundaries()`뿐이다. 따라서 B2/WS는 B1에 의존하지 않는다. 다만 cumulative patch에서 engine/pipeline/policy/test hunk가 서로 겹치므로 B1 hunk를 단순 파일 단위로 reverse하면 안 되고 B2-only 상태에 WS ctor/predicate/test만 선택 합성해야 한다. |
| 6 | TSan·관련 suite | **기존 로그 통과 / W-SD2-4** | 보존 로그 `SD-2-related-run1..3.log`와 `SD-2-tsan-27.log`에서 관련 27/27×3 및 TSan 27/27, sanitizer report 0을 확인했다. `test_zmp_ws_wss`, `test_asio_ws`, policy unit도 포함된다. 다만 이 로그는 B1이 포함된 누적 최종 patch 대상이며, B1 제거 후 정확한 채택 diff의 증거는 SD-3 검증이 소유해야 한다. |
| 6 | hotpath | **기존 로그 통과 / 범위 W** | `SD-2-hotpath-final.log`의 5셀 비율은 0.9611/0.9993/1.0461/1.0230/1.0142로 모두 PASS다. 그러나 `hotpath_reference.json` hunk는 D-B291의 채택 목록(app+B2+D-f+WS)에 없다. 별도 D-B277 기준 변경으로 명시해 착지하거나 SD-3 채택 patch에서는 제외해야 한다. |
| 6 | public/ABI·플랫폼 | **통과** | patch에 `core/include/**`와 `core/src/libzlink.vers` 변경은 없다. 채택 부분은 기존 internal `has_message_boundaries()`, `async_writev()`와 표준 C++만 사용한다. Beast const-buffer-sequence `async_write`도 base에 이미 있던 API이므로 새 Windows/macOS 또는 Boost-version 의존 API가 없다. B1의 새 transport virtual/`async_wait`는 기각 범위다. |

## B-SD2-1 상세 — WS 자동 gather가 bounded encoder batch를 대체함

`use_gather_write_for()`의 세 조건은 “두 buffer를 한 transport operation에 낼 수 있다”는 능력은
증명하지만, 그것이 ZMP의 WS batch owner를 대체해도 된다는 조건은 증명하지 않는다. WebSocket
message boundary는 ZMP record boundary가 아니며, §9의 owner는 현재 준비된 ZMP byte를 target까지
모으는 encoder다. 새 gate는 transport boundary를 이유로 이 owner를 `prepare_gather_output()`의
단일-message 경로로 바꾼다.

구체적인 반례는 다음 두 가지다.

1. Encoder target이 128 KiB인 connection에 64 KiB body frame과 target 잔여에 들어가는 작은
   frame이 모두 준비되어 있어도 gather는 첫 `_next_msg`만 꺼내 Beast write를 시작한다. 후속
   frame은 completion 뒤 별도 write가 된다.
2. Body가 128 KiB보다 커도 gather에는 `stream_encoder_write_target_max` 또는
   `zmp_send_batch_max_size()` 검사가 없어 header+body 전체를 한 composed write에 제출한다.

둘 다 미래 traffic을 기다리는 문제는 아니지만 “현재 준비된 byte를 기존 상한까지 모으는 bounded
batch”를 보존했다는 주장에는 반례다. 64 KiB gate 3회 PASS는 특정 크기 비율을 확인할 뿐 이 두
경로를 검사하지 않는다.

수정은 WS message와 ZMP frame을 같은 경계로 계약화하는 방식이어서는 안 된다. 기존 encoder batch
owner 안에서 header/body copy를 피하면서도 준비된 후속 frame을 target까지 포함하는 buffer sequence를
만들거나, 적어도 자동 gather admission이 현재 target/max와 준비된 batch를 우회하지 않음을 증명해야
한다. 128 KiB 초과 단일 frame과 `64 KiB + small ready frame`의 실제 Beast submission 수·wire decode를
고정하는 회귀 검사가 필요하다.

## 신규 B/W/S

| ID | 등급 | 내용 |
|---|---|---|
| B-SD2-1 | **B** | message-boundary를 근거로 단일-message gather를 기본화해 ZMP §9의 bounded multi-frame batch owner와 max를 우회한다. |
| W-SD2-1 | **W** | “header/body가 서로 다른 WebSocket frame이면 위반”이라는 보고서/D-B291 근거는 ZMP byte-carrier 계약과 반대다. WS 수정은 wire 결함 B가 아니라 내부 batching 성능 변경으로 다시 분류해야 한다. |
| W-SD2-2 | **W** | 1 MiB는 benchmark `rcvbuf`에서 유도된 decoder target max이지 런타임 기본값 또는 connection RSS hard cap이 아니다. +140.77 MiB 관측은 유효하나 “상한의 14%”는 capacity bound 증명이 아니다. |
| W-SD2-3 | **W** | 코드·0.17.3 tag로 양쪽 framing 호환은 확인되지만 실제 mixed-version 및 제3 구현 송수신 실행 증거는 없다. |
| W-SD2-4 | **W** | TSan/관련 suite는 B1 포함 patch 결과이며, hotpath reference hunk는 D-B291 채택 목록 밖이다. SD-3의 정확한 합성 diff에서 재검증·범위 분리가 필요하다. |
| S-SD2-1 | **S** | B2 1-hit 성장, 기존 max/shrink, async/speculative byte 갱신에서 stale-short 성장 반례를 찾지 못했다. |
| S-SD2-2 | **S** | app의 RID별 조립과 exact 1-frame zero-copy, D-f 세 accessor 제거, RAW STREAM/TCP ZMP 기본 경로 불변은 정적으로 적합하다. |
| S-SD2-3 | **S** | `asio_stream_fastpath_policy.hpp`의 “03-io-thread §4 one prepared buffer” 주석은 실제 §4가 튜닝 가이드이므로 근거 절 번호가 stale하다. 동작 차단은 아니며 후속 주석 정리 대상이다. |

소유 계층: B2 read/write target은 Core Asio engine/policy, ZMP batching과 WS carrier write는 Core
ZMP encoder/WS transport, benchmark frame 조립은 계측 app이 각각 소유한다.

변경 분류: app=B 기존 계측 결함, B2=B 기존 성장 결함, D-f=A 무효 compatibility 입력 제거.
WS gate는 wire 결함 B가 아니라 내부 batching 성능 변경이며, 현재 구현은 B-SD2-1 때문에 채택할 수 없다.

규칙 수: B2는 “연속 2 hit + reset”을 “full request 1회 → 2배/max clamp” 하나로 줄인다.
WS gate는 반대로 bounded encoder batch 외에 단일-message gather owner를 기본 경로로 추가하므로
결과 규칙을 하나로 수렴시키지 못한다.

차단 항목 수 / 채택 가능 여부: 1 / 현재 채택 부분은 채택 불가(B-SD2-1 해소 후 재검토)
