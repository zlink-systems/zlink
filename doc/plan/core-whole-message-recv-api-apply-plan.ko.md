---
title: "whole-message recv 공개 API — 적용 plan (문서·코드·perf 반영 대상과 순서)"
---

# whole-message `recv()` 공개 API — 적용 plan

> 설계는 draft(`doc/draft/core-whole-message-recv-api.ko.md`)가 소유한다. 이 문서는 **무엇을 어디에 어떤 순서로 반영하는지**
> — Core spec·guide, 각 바인딩 spec·guide, Core 코드, 바인딩 코드, perf 하네스의 **구체적 대상 파일**과 절차·검증을 담는다.
> (라인 앵커는 조사 시점 기준이며 편집 직전 재확인. spec 문서는 보호 경로 → 감독(머신 B)이 직접, 사용자 승인 범위. en/ko 동기화.)

## 0. 범위·전제

> **2026-09-10 범위 확대.** send도 whole-message로 신설하고 **part 단위 공개 API는 제거**한다(draft §7).
> 근거는 draft §7.2의 저장소 전수 조사다 — lazy send 없음, part 단위 수신 의존 없음(Core가 첫 part 공개 전에
> record 전체를 버퍼링), STREAM·XPUB은 이미 part 단위가 아님, `framework/languages`의 part API 호출 0건.
> 아래 §0~§9의 "recv"는 **send·recv 양방향**으로 읽는다. 단계별 대상은 §10에 더한다.

- 신설: whole-message `recv`(ROUTER/PAIR/DEALER 우선) — draft §3. ~~`*_recv_part`는 유지.~~
  → **part 계열 공개 API 제거**(draft §7.4). 제거 대상은 §10.1.
- 부수: `reqrep::recv_router_message_direct` **네이밍 정정**(Core 내부, 저위험, perf 영향 없음).
- 원칙: 공개 계약·thread-safety·측정 의미 보존, 수치 조작 금지(§7.0.1), 공개 표면 단순성 유지.

## 1. 반영 순서 (단계 게이트)
1. (draft 검토·승인)
2. **네이밍 정정**(Core 내부) — 별도 커밋, 빌드/테스트 통과.
3. **Core spec** 반영(§2 대상표) — 감독 직접.
4. **Core 코드** 구현(§3) — 단위/계약 테스트.
5. **Core guide** 반영(§4) — 사용법·예제.
6. **바인딩 코드** 전환(§5) — 각 바인딩 내부 수신을 whole-message로, 계약 테스트.
7. **바인딩 spec·guide** 반영(§6) — 각 언어 문서.
8. **perf 하네스** 적용(§7) — relay/echo recv를 whole-message로, 의미 보존.
9. **perf 재측정·기록**(§8) — routed before/after, §7.7 채택/회귀 확인, perf 계획서 기록.

## 2. Core spec 수정 대상 (`core/doc/spec/**`, ko+en)
| 파일 | 반영 지점 | 넣을 내용 |
|------|-----------|-----------|
| `core/doc/spec/core/socket/README.ko.md` / `.en.md` | §2 스레드 안전성, §3 Pull 수신 표(일반 DATA 수신 함수 표) | whole-message `recv`/`router_recv` 행 추가, single-consumer·record 원자성·borrowed rid 수명 규정, recv_part와 혼용 규칙(EBUSY 등) |
| `core/doc/spec/core/socket/07-router.ko.md` / `.en.md` | ROUTER 수신 계약 | `zlink_router_recv`(rid+reply_token+parts 배열) 시그니처·의미·capacity·DONTWAIT·REQUEST 시 reply target |
| `core/doc/spec/core/socket/06-dealer.ko.md` / `.en.md` | DEALER 수신 | `zlink_recv` 적용(멀티파트 record) |
| `core/doc/spec/core/socket/01-pair.ko.md` / `.en.md` | PAIR 수신 | `zlink_recv` 적용 |
| `core/doc/spec/core/02-message.ko.md` / `.en.md` | 멀티파트·ownership | parts 배열 소유권(각 슬롯 caller-소유, 각자 close), capacity 초과·부분 record 규칙 |
| (2차) `socket/03-sub`,`05-xsub`,`04-xpub` | topic/subscribe recv | 필요 시 whole-message 확장 |

## 3. Core 코드 수정 대상 (`core/`)
| 파일 | 변경 |
|------|------|
| `core/include/zlink/socket/api.h` (recv 계열 `:285~`) | 공개 `zlink_recv`·`zlink_router_recv` 선언 추가(draft §3.3), 문서 주석 |
| `core/src/api/socket/socket_request_reply_router_api.cpp` | `zlink_router_recv` 구현(내부 whole-record pull 직접 사용), `router_recv_part_impl`과 공유 로직 정리 |
| `core/src/api/socket/socket_message_api.cpp` | `zlink_recv`(PAIR/DEALER) 구현 |
| `core/src/api/socket/socket_request_reply_runtime_io.cpp` (`recv_router_message_direct :951`) | **네이밍 정정**(reqrep→일반 router recv), whole-message 공개 경로가 재사용 |
| `core/src/api/socket/socket_request_reply_internal.hpp` (`:539`) | 선언 rename |
| `core/src/api/socket/part_helper_*` | recv_part 어댑터는 유지; whole-message 경로는 시퀀스 버퍼·mutex 우회 확인 |
| Core 단위/계약 테스트 | whole-message recv: part 수·순서·rid/token·capacity 초과·DONTWAIT·close·record 원자성·recv_part 혼용 |

## 4. Core guide 수정 대상 (`core/doc/guide/**`, ko+en)
| 파일 | 넣을 내용 |
|------|-----------|
| `core/doc/guide/02-core-api.ko.md` / `.en.md` | recv 소개에 whole-message `recv` 추가, recv_part와 언제 무엇을 쓰나 |
| `core/doc/guide/03-0-socket-patterns.ko.md` / `.en.md` | routed 수신 예제에 whole-message recv 사용 |
| `core/doc/guide/03-6-proxy.ko.md` / `.en.md` | relay/echo 예제를 whole-message recv로 |
| `core/doc/guide/11-thread-safety.ko.md` / `.en.md` | recv single-consumer 계약 그대로임을 명시(변경 없음 확인) |

## 5. 바인딩 코드 수정 대상 (`bindings/<lang>/`)
| 바인딩 | 변경(내부 수신을 whole-message로) |
|--------|-----------------------------------|
| cpp | 수신 구현이 `Received`/parts를 채우는 경로를 whole-message recv로. 재사용 배열/슬롯. 계약 테스트 |
| dotnet | `SocketKernel.ReceiveCore`/`Received` 채우기를 whole-message recv로(part 루프·per-part 경계 제거, 배열 pool). 계약 테스트 |
| node | addon 수신 + `Received` 채우기를 whole-message recv로(N-API 경계·envelope 할당 감소). 계약 테스트 |
| java | FFM 수신 + `Received` 채우기를 whole-message recv로. 계약 테스트 |
- 공개 바인딩 표면 최소 변경(이미 컬렉션 반환). 필요 시 whole-message 편의 메서드 노출 여부는 각 바인딩에서 판단·문서화.

## 6. 바인딩 spec·guide 수정 대상 (ko+en)
| 파일 | 넣을 내용 |
|------|-----------|
| `bindings/doc/spec/{cpp,dotnet,java,node}/README.ko.md` / `.en.md` (recv/Received 절) | 내부 수신이 Core whole-message recv를 사용함, `Received` 컬렉션 채우기 계약(소유권/부분/실패), 노출 시 편의 메서드 |
| `bindings/doc/guide/{cpp,dotnet,java,node}/index.ko.md` / `.en.md` (수신 절) | recv 사용 예제(멀티파트를 한 번에 받는 `Received`), 소유권·close, recv_part류와의 관계 |

## 7. perf 하네스 (대부분 변경 없음 — 이미 recv(Received) 사용)
- **바인딩 perf(cpp/dotnet/java/node)는 변경 불필요.** 이미 바인딩 라이브러리 `recv(Received)`를 호출한다(§5의 내부 구현이 바뀌면 **하네스 코드
  변경 없이 자동 반영**). 확인만: `cpp perf_multi_routed_relay.hpp:172 server.recv(received)`, `node perf_multi_runtime.ts:207 socket.recv(received)`,
  `dotnet PerfMultiRoutedRelayServer.cs:280 socket.Recv(result)`, `java PerfMultiRoutedRelay.java:90 server.recv(received)`.
- **C 레퍼런스 perf(`bindings/c/perf`)만 선택적 정리**: 신설 Core `zlink_recv`/`zlink_router_recv`로 바꾸면 `zlink_router_recv_part`+has_more
  수동 조립을 제거해 단순화(예: `perf_multi_socket_reqrep.hpp:916`, `perf_multi_relay_server.hpp`). 측정 의미(§7.0.1)·실패/retry/metric/HWM/client·duration 불변.
- go/rust/python은 이번 범위 밖(추후). 측정은 cpp/node/java/dotnet만(사용자 지정).

## 8. perf 재측정·검증
- 대상: multi routed(DEALER_ROUTER/ROUTER_ROUTER, SENDSEND·REQREP), tcp, runs=3, Core 최신(0.17.5), C baseline 재사용.
- 판정: 목표 대비 갭 축소(특히 작은 size), **비대상(PAIR/PUBSUB·대형·타 pattern) 회귀 없음**(§7.6/§7.7). 회귀 시 해당 변경 revert.
- 결과를 `doc/perf/perf/bindings-0.17.5/bindings-library-performance-improvement-plan-core-0.17.5.ko.md` §11에 before→after로 기록.
- 단위·계약 테스트 전체 통과, 공개 표면 단순성·측정 의미 보존을 최종 리뷰.

## 9. 완료 기준
- Core: 공개 `recv`/`router_recv` 시그니처가 spec·헤더·구현·테스트에서 일치, recv_part 병존, 네이밍 정정 반영.
- 문서: Core spec·guide, 바인딩 spec·guide(ko/en) 모두 whole-message recv 반영, en/ko 동기.
- 바인딩: 내부 수신 whole-message 전환, 계약 테스트 통과, 공개 표면 파편화 없음.
- perf: routed 목표 갭 축소·비대상 무회귀, before/after 기록, 채택분 커밋·푸시.


## 10. 범위 확대 — send whole-message 신설과 part API 제거 (2026-09-10)

설계와 조사 근거는 draft `doc/draft/core-whole-message-recv-api.ko.md` §7이 소유한다. 여기서는 반영 대상만 적는다.

### 10.1 제거 대상과 신설 대상

| 지금 | 어떻게 |
|---|---|
| `zlink_send_part`, `zlink_send_part_rid`, `zlink_request_part`, `zlink_reply_part`, `zlink_publish_part` | **제거.** parts 배열 + count를 받는 whole-message send로 대체 |
| `zlink_recv_part`, `zlink_router_recv_part`, `zlink_subscribe_part` | **제거.** §3의 whole-message recv로 대체(`zlink_subscribe_part`는 §3 목록에 빠져 있었다 — draft §7.2 Q3) |
| `zlink_xpub_recv_part` | **유지.** `zlink_msg_t`를 받지 않는 구독 이벤트 리더이며 이름만 `_part`다 |
| `zlink_stream_recv_packet` | **유지.** header/body 고정 2슬롯 framing이라 일반 배열 API와 의미가 다르다 |

### 10.2 먼저 정할 계약 셋

1. **`count > capacity`**: record를 잃지 않으면서 필요한 개수를 알려주는 방식. 지금의 드레인 루프에는 이 실패가
   없으므로 새로 정의한다. §3의 capacity 초과 규칙과 같은 형태로 맞춘다.
2. **STREAM의 길이 0 part 의미 보존**: 유효한 RID로 보내는 빈 part는 "그 peer를 끊는다"는 별도 의미다
   (`core/doc/spec/core/socket/08-stream.en.md:151-153`). 1-element 배열이 이 의미를 유지해야 한다.
3. **재시도 단위**: 실패한 제출은 **whole-record 재시도**다. part 단위 부분 재시도는 없어진다
   (지금도 실패한 `FINAL`은 staged prefix를 버린다 — `README.en.md:1078`).

### 10.3 §1 단계 게이트에 더하는 것

- 2단계(네이밍 정정) 뒤, **Core spec에 send whole-message와 제거를 함께 반영**한다(§2 대상표에 send 절 추가).
- 4단계(Core 코드)에서 신설과 제거를 **같은 커밋 계열**로 처리하되, 심볼 제거는 마지막 커밋으로 분리해
  되돌리기 쉽게 한다. `libzlink.vers`도 함께 정리한다.
- 6단계(바인딩 코드)에서 **각 바인딩의 내부 send 루프를 배열 한 번 호출로 교체**한다. 공개 시그니처는 불변이다.
- 8단계(perf 하네스)에 `bindings/c/perf/single/common/perf_single_reqrep.hpp:635,647-664`를 더한다. 빈 `FINAL`만
  재시도하는 유일한 경로이며 whole-record 재시도로 바꾼다(draft §7.2 Q1).

### 10.4 작업 규모

draft §7.3의 표를 따른다. 요약하면 **Core 내부 호출자 0건, `framework/languages` 0건**이고, 비용은
`core/doc` 약 1,000줄과 `core/tests` 약 450줄, 바인딩 내부 약 180줄로 거의 기계적이다.

### 10.5 이 확대가 푸는 문제

part 단위 표면은 "한 record의 첫 part부터 FINAL까지 같은 thread"라는 계약을 만든다
(`core/doc/spec/core/socket/README.ko.md` §2). Framework Java의 send 지연 수정 중 관측된 `BUSY` 반복이
이 계약과 부딪힌 결과였다. 표면을 한 번의 호출로 바꾸면 미완성 record 상태 자체가 없어지고, 그에 딸린
thread 계약·오류 경로·부분 재시도 규칙이 함께 사라진다.
