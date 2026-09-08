# review-SD-4 — B-SD2-1 해소 재검증

검토 기준은 base `84d25131a6`과 고정 artifact
`/home/hep7hep7/project/zlink-work/all-artifacts/SD-4.patch`
(SHA-256 `b94c8dd24b1091d238a5fce0b4644c9cc0faf761a277c5a48fe8688eeed10ed1`)다.
`review-sd2.md`, `core-rf-SD-4-report.md`, post-patch source와 기존 로그만 읽었다. 소스·스펙을
수정하지 않았고 빌드·테스트·benchmark를 실행하지 않았다. 작성한 파일은 이 보고서와
`progress-review-sd4.md`뿐이다. B는 채택 차단, W는 비차단 경고·증거 한계, S는 확인 결과다.

## 결론

SD-2가 넣은 message-boundary 기본 gather는 제거됐고 TCP ZMP의 기존 env opt-in 의미도
유지된다. 그러나 대체한 bounded pointer gather가 body가 현재 target을 실제로 넘는지 검사하지
않는다. Encoder target이 128 KiB일 때 먼저 준비된 64 KiB body도 즉시 `[header, body]`로
제출하므로, 뒤에 준비된 small frame을 같은 batch에 포함하지 못한다
(`asio_engine.cpp:1262-1306`, `ws_batch_policy.hpp:28-41`). 이는 review-SD-2의 첫 반례와
`01-zmp.ko.md:489-492`의 현재 준비된 byte를 target까지 모으는 규칙을 그대로 위반한다.

추가된 첫 unit은 입력 순서를 원 반례의 `64 KiB → small`이 아니라 `small → 64 KiB`로 바꾸고
pointer gather가 선택된다는 사실만 검사한다(`unittest_asio_write_turn_policy.cpp:329-341`).
두 번째 unit도 max 초과를 policy 반환값으로만 검사하며 기존 encoder 분리와 실제 Beast write
수를 관찰하지 않는다(`:343-353`). 따라서 B-SD2-1은 해소되지 않았다.

## 항목별 판정

| 항목 | 판정 | 근거 파일:행과 판단 |
|---|---|---|
| SD-2 단일-message gather 원복 | **통과** | `asio_stream_fastpath_policy.hpp:139-143`은 `protocol header && gather 지원 && ZLINK_ASIO_GATHER_WRITE`만 허용한다. Constructor도 message-boundary 값을 policy에 넘기지 않는다(`asio_engine.cpp:144-151`). SD-3의 자동 gate 인자와 predicate는 SD-4에서 제거됐다. |
| TCP ZMP env opt-in·RAW 기본값 | **통과** | `ZLINK_ASIO_GATHER_WRITE`가 켜진 TCP ZMP만 기존 gather policy에 들어가며 unit이 이를 고정한다(`unittest_asio_write_turn_policy.cpp:309-326`). RAW STREAM은 gather header를 만들지 않아 false다. Production env나 기본값을 새로 추가하지 않았다. |
| (a) 128 KiB target 안의 `64 KiB → small` | **차단 — B-SD4-1** | Admission은 `body_size >= 64 KiB`와 max만 보고 target-crossing 조건을 검사하지 않는다(`asio_engine.cpp:1270-1277`, `ws_batch_policy.hpp:34-41`). 첫 64 KiB frame이 empty copy batch에서 pointer gather로 batch를 닫으므로 loop가 `break`하고(`asio_engine.cpp:1294-1306`), 뒤의 small frame은 다음 write로 밀린다. 기존 copy batch 한 write가 아니다. |
| (b) target 초과·128 KiB max 이내 body | **코드 통과** | 예를 들어 initial target 16 KiB, body 64 KiB이면 header가 target에 들어가고 operation 합계가 128 KiB 이내여서 true다(`ws_batch_policy.hpp:34-41`, unit `:343-350`). Header를 copy batch 끝에 붙이고 body pointer를 두 번째 buffer로 저장한 뒤 한 `async_writev()`로 제출한다(`asio_engine.cpp:1278-1306,776-795`). |
| (c) 128 KiB max 초과 | **코드 통과 / test 불충분** | operation 합계가 max를 넘으면 helper가 false이고(`ws_batch_policy.hpp:36-41`), engine은 message를 기존 encoder에 넘겨 target 단위로 분리한다(`asio_engine.cpp:1309-1326`). Unit은 `maximum + 1` body가 false인지만 검사해 fallback 분리와 write 수는 고정하지 않는다(`unittest_asio_write_turn_policy.cpp:351-352`). |
| review-SD-2 반례 test | **실패** | 첫 test는 small frame이 이미 copy batch에 있는 반대 순서를 사용하고 `[batch, body]` 허용을 기대한다(`unittest_asio_write_turn_policy.cpp:329-341`). 원 반례인 `64 KiB → small`의 copy batch 한 write를 검사하지 않는다. 두 test 모두 순수 helper unit이라 `prepare_output_buffer()`와 Beast submission 수·wire decode를 관찰하지 않는다. |
| Body pointer 수명 | **통과** | Body는 기존 `_pipeline.tx_msg`가 소유하고 pointer를 `_pipeline.gather_body`에 보관한다(`asio_engine_pipeline.hpp:59,65-70`, `asio_engine.cpp:1300-1304`). Beast는 buffer sequence 객체를 복사하지만 underlying memory를 completion까지 요구한다(`boost/beast/websocket/stream.hpp:2578-2606`). `finish_gather_output()`만 `tx_msg`를 close/re-init한다(`asio_engine.cpp:797-812`). |
| 부분 write·오류·취소·disconnect 반환 순서 | **통과** | WS/WSS `async_writev()`는 두 buffer를 한 Beast composed `async_write`에 넘긴다(`ws_transport_common_internal.hpp:237-265`). Beast 내부 partial write가 끝나거나 오류가 날 때 engine completion이 호출된다. 성공과 non-aborted 오류는 body 반환 후 다음 write 또는 `error(connection_error)`로 진행한다(`asio_engine.cpp:1048-1075`). 종료·취소는 callback guard를 먼저 무효화하고 transport를 닫은 뒤 지연 파괴에서 `tx_msg`를 반환하는 기존 순서를 재사용한다(`asio_engine.cpp:340-392,181-207,1812-1821`). |
| TSan 관련 suite | **통과 / W-SD4-2** | `sd1/core/build-tsan/Testing/Temporary/LastTest.log`는 source 수정과 TSan binary 갱신 뒤 2026-09-09 00:52~00:53에 관련 27/27을 기록하며 `ThreadSanitizer`·`data race` 표식은 0건이다. `CMakeCache.txt`의 `-fsanitize=thread -fno-omit-frame-pointer -fPIE`도 확인했다. 다만 immutable artifact 로그가 아니라 작업 worktree의 `LastTest.log`다. |
| RAW STREAM·TCP ZMP byte 경계 | **통과** | 새 pointer branch는 `zmp_transport_has_message_boundaries() && supports_gather_write()`로 제한된다(`asio_engine.cpp:1271-1274`). 전자는 STREAM socket을 명시적으로 제외하고(`asio_engine.cpp:413-416`) TCP는 message boundary가 없으므로 WS/WSS ZMP만 진입한다. 공통 제출 helper와 `process_output()` 통합은 기존 buffer를 같은 순서로 제출하며 wire byte를 만들지 않는다. |
| 준비된 byte만·대기 없음 | **통과** | `prepare_output_buffer()`는 현재 `_next_msg`가 즉시 제공하는 message만 target까지 읽고, 실패하면 loop를 끝낸다(`asio_engine.cpp:1262-1268`). Future traffic 대기나 timer·retry가 추가되지 않았다. 단, (a)의 조기 batch 종료는 별도 차단이다. |
| 새 상태·옵션·env | **통과** | Persistent field는 추가되지 않았고 기존 `async_gather`, `tx_msg`, `gather_body`를 재사용한다. 새 production option/env는 0이며 D-f의 사용되지 않던 STREAM gather env accessor 3개는 남아 있지 않다. 추가된 것은 공통 제출 method와 순수 policy helper다. |
| W-SD2-2 보고서 표현 | **통과** | `core-rf-SD-4-report.md`는 decoder/RSS를 connection당 1 MiB hard cap으로 부르지 않는다. 이 보고서의 “128 KiB max”는 `ws_batch_policy.hpp:20-23`의 WS send batch 상한을 가리켜 문맥이 다르다. |
| B2·D-f·계측 app의 SD-3 이후 변경 | **통과** | SD-3/SD-4 patch의 app target blob은 모두 `c4bceac7d3`, pipeline target blob은 모두 `28d1a7754c`다. B2 1-hit 성장 hunk와 D-f accessor 제거도 동일하다. App은 exact 1-frame만 원본 message로 보내고 나머지는 RID별 buffer에서 조립한다(`test_scenario_stream_zlink.cpp:87-103,208-284`). SD-3 이후 변경은 gather 원복·bounded batch·관련 unit과 제출 helper뿐이다. |
| WS/WSS gate 3회 | **통과** | 보존 report 세 건은 각 12/12 cell, fail/skip/unsupported 0이다. `core-rf-SD-4-report.md:73-79`의 ws Q64/Q1은 1.171861/1.257310/1.172784, wss는 0.931437/1.067634/1.082682로 모두 0.80 이상이다. 다만 `progress-SD-4.md`는 target 미초과 64 KiB도 batch를 닫도록 바꾼 뒤 이 결과가 나왔다고 기록하므로 B-SD4-1의 반증은 아니다. |
| hotpath 5셀 | **W-SD4-3** | `core/build/Testing/Temporary/LastTest.log`와 보고서 `:85-97`은 4 PASS, `dealer_router_reqrep_inproc` 0.9496 한 건 FAIL이다. WS/WSS branch와 무관한 inproc cell이고 reference는 base와 같지만, “hotpath gate 통과” 증거로 사용할 수는 없다. |
| Public header·version script | **통과** | Patch 파일 목록에 `core/include/**`, `core/src/libzlink.vers`, `core/tests/perf/hotpath_reference.json`이 없다. Public API/ABI와 reference는 바뀌지 않았다. |

## 신규 B/W/S

| ID | 등급 | 내용 |
|---|---|---|
| B-SD4-1 | **B** | Target-crossing 검사가 없어 128 KiB target에 들어가는 첫 64 KiB frame도 pointer gather로 batch를 닫는다. 뒤의 준비된 small frame을 같은 write에 포함하지 못해 B-SD2-1의 첫 반례와 ZMP §9를 위반한다. |
| W-SD4-1 | **W** | 반례 unit은 원 입력 순서를 뒤집고 pure policy 반환값만 검사한다. 실제 `prepare_output_buffer()`의 copy/gather 선택, Beast write 수와 max 초과 fallback을 검사해야 한다. |
| W-SD4-2 | **W** | TSan 27/27·sanitizer 0 근거는 확인됐지만 고정 artifact에 별도 로그로 보존되지 않았다. |
| W-SD4-3 | **W** | hotpath는 reference 무변경 상태에서 WS 변경과 무관한 inproc cell 하나가 0.9496으로 실패했다. |
| S-SD4-1 | **S** | 기존 `tx_msg` 소유와 Beast completion 경로를 재사용해 body pointer 수명, partial write와 disconnect 반환 순서에 새 결함을 찾지 못했다. |
| S-SD4-2 | **S** | SD-2 자동 gather 원복, TCP env opt-in·RAW 비활성, max 초과 기존 encoder fallback, 새 production 상태·옵션·env 0을 확인했다. |
| S-SD4-3 | **S** | B2·D-f·계측 app은 SD-3 이후 그대로이며 WS/WSS gate 3회는 모두 기준을 통과했다. Public header·version script·hotpath reference도 변하지 않았다. |

## 소유 계층·스펙·교차언어·분류

- 소유 계층: 준비된 ZMP byte의 target과 batch 제출 단위는 Core Asio encoder의
  `prepare_output_buffer()`가 소유한다. WS/WSS transport는 buffer sequence를 Beast에 전달한다.
- 스펙 조항: `01-zmp.ko.md:384-392`는 WS/WSS를 byte carrier로, `:489-492`는 현재 준비된
  byte를 `out_batch_size`까지 모은 bounded batch 한 번의 Beast write로 규정한다.
- 교차언어 대조: C/C++/.NET/Java/Kotlin binding은 같은 native Core engine을 사용하므로 언어별
  보상 경로는 없다. 이번 finding도 모든 binding의 WS/WSS ZMP 송신에 공통이다.
- 변경 분류: 의도는 B(기존 batching 성능 결함)이지만, 현재 구현은 target rule을 지키지 않아
  C(성능 gate를 위한 조기 batch 종료)로 남는다.
- 규칙 수: batch owner는 2개에서 1개로 줄었지만, 그 owner 안에 “64 KiB 이상이면 target 잔여와
  관계없이 닫는다”는 예외가 추가됐다. 요청한 “target을 넘을 때만 body pointer를 두 번째
  buffer로 둔다”는 단일 규칙으로 수렴하지 않았다.

차단 항목 수 / 채택 가능 여부: 1 / 채택 불가(B-SD4-1, B-SD2-1 미해소)
