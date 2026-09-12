# Binding 라이브러리 성능 최적화 가이드

언어별 binding 라이브러리 hot path에 **어떤 최적화가 적용됐고, 무엇이 시도됐다 기각됐는지**를
한눈에 보기 위한 유지보수용 참고 자료다. 공개 API/동작 계약을 정하는 spec이 아니다.

- 판정 수치(통과/보류 비율)와 측정 범위의 근거는 각 캠페인 계획서(`perf/bindings-<ver>/…`)와 그
  `decisions.ko.md`다. 이 문서는 그 결론만 요약한다.
- **소스 대조 검증**: §3 매트릭스의 표시는 2026-09-11 각 언어 바인딩 소스를 직접 읽어 확인한 상태다.
  `◐`(형태 드리프트)·`✗`(기록됐으나 소스에 없음)는 §4 언어별 현황에 근거를 남겼다.
- 계약 원문: `core/doc/spec/core/socket/README.ko.md` "Part send" 절, `core/doc/spec/core/05-polling.ko.md`.

Core 0.17.0부터 DONTWAIT send 완료 모델: 거절 시 `BACKPRESSURED` + 대기 토큰, credit 회복 시
`ZLINK_COMPLETION_WRITABLE`(callback 없음, pull 방식). 이 문서는 현 계약에서 유효한 기법만 싣는다.

## 1. 판정 기준

- 최적화는 **public API·message ownership·Future/Task/CompletionStage 수명을 바꾸지 않는다.** pool은
  외부에서 identity를 관찰할 수 없고 종료 시점이 분명한 내부 상태에만 쓴다.
- **효과 확인**: 같은 조건(pattern·transport·size·clients·duration·같은 Core binary)에서 전/후 측정해
  5% 이상 개선. **구조 개선**: 소유권/수명을 단순화했으나 독립 처리량 이득은 미확정 — 이득이 없어도
  채택 가능(계약·측정 의미 보존이 조건). **기각**: 측정으로 효과 없거나 안전성을 깨서 되돌린 것.
- binding은 **C baseline 대비 비율**로 판정한다. 언어 간 절대값 비교는 순위가 아니라 병목 분리용 표본.
- 러너(측정 하네스) 정합은 라이브러리 개선이 아니다 — library 효과와 합산하지 않는다.
- 표준 측정: 각 binding `perf/run_benchmarks.sh`(single)·`run_benchmarks_multi.sh`(multi), 규칙은
  `PERF_POLICY.md`.

## 2. 최적화 아이템 카탈로그

hot path를 전송 1건 기준 단계로 나눈 아이템 목록이다. ID는 §3 매트릭스·§4에서 참조한다.

### 즉시 성공 send (DONTWAIT → OK, completion_id 0)
| ID | 아이템 | 근거·효과 |
|---|---|---|
| S1 | OK send에 completion entry·Promise/Future·waiter map을 만들지 않는다(토큰 반환 뒤에만) | Node: 성공마다 entry·Promise·map 2회 등록 → DEALER_ROUTER 28k, 제거 후 145k |
| S2 | OK send마다 native poller·goroutine·thread를 만들지 않는다 | Go 81.5k→190k; Rust send마다 poller 생성/파괴 제거 |
| S3 | payload를 바이트로 복사·보관하지 않는다. 재전송 snapshot은 거절 시점에만, `zlink_msg_copy`(refcount 공유) | C multi helper 전송당 2회 복사 제거; Python 7.9k→20k; .NET snapshot을 거절 시점으로 |
| S4 | socket 배타 락을 submit 전체에 걸지 않는다(shared submit / exclusive close) | Rust: submit gate Mutex→RwLock |
| S5 | 2-part staging은 heap 대신 inline/stack/scratch | C++ inline, .NET stack+ArrayPool, Java scratch 재사용, Node inline 8-part |

### 거절된 send (BACKPRESSURED + 대기 토큰)
| ID | 아이템 | 근거·효과 |
|---|---|---|
| R1 | 토큰·context·RID를 키로 O(1) 조회(선형 스캔 제거) | Python O(n)→단일 key; Node POLLOUT 선형 순회→카운터; .NET HashSet 할당 제거 |
| R2 | 등록→recheck 창에서 drain된 WRITABLE을 보류했다 등록 시 재생 | Rust: never-reused context counter + 보류 재생 |

### completion drain·wake
| ID | 아이템 | 근거·효과 |
|---|---|---|
| D1 | 이벤트 없을 때 timeout-0 poll/`yield`/`setImmediate` 재예약 금지. 실제 wake 소스(blocking `zlink_poller_wait` 또는 public poller)에서 깨운다 | Rust spin(CPU 100%)→reactor+public Poller 172k→259k; 전 언어 동일 spin 제거 |
| D2 | drain owner는 한 곳, NO_DATA까지 비운다. REQUEST completion과 stray WRITABLE을 분리(익명 WRITABLE 스킵) | C REQREP 러너: `completion_id_out=NULL` send가 남긴 WRITABLE을 REQUEST drain이 스킵 |
| D3 | POLLOUT은 socket 전체 level hint다. 대상 WRITABLE record 없이 POLLOUT만 오면 관심 내리고 POLLCOMPLETION 대기 | C DEALER_DEALER 스핀 방지; C++ 빈 drain 제거 |
| D4 | runtime owner 스레드는 토큰이 생긴 뒤 시작하고 idle이면 종료 | Rust reactor ~100ms idle 종료; Go bounded blocking poll |
| D5 | EINTR은 종료가 아니다. 종료 결과(ETERM/ESHUTDOWN)에서만 대기자를 실패시킨다 | .NET: EINTR/EBUSY에서 대기자를 실패시켜 payload가 버려졌던 결함 |

### 수신·reply
| ID | 아이템 | 근거·효과 |
|---|---|---|
| V1 | REQ/REP server는 받은 컨텍스트/토큰을 재사용해 직접 reply(`Message→bytes→Message` 왕복 금지) | Java RR SENDSEND TCP +19.5%; C++·Node direct, Py·Go·Rust는 owner-aware `zlink_msg_copy` |
| V2 | 수신 wrapper·native header는 재사용하고 payload 존재를 캐시 | C++ DD TLS 89.2→93.2%; .NET PUB/SUB 68.0→84.4%; Node routed frame pool 64B 79.8→111.9 Kmsg/s |
| V3 | 이미 native `msg_t`를 소유한 part는 arena·복사 없이 Core에 넘긴다 | Java request zero-copy 7.2~21.1% |
| V4 | 기본 wrapper 생성자는 곧 덮어쓸 native storage를 초기화하지 않는다 | C++ 84.6→95.5% |

### 오류·종료 경계
| ID | 아이템 | 근거 |
|---|---|---|
| E1 | WRITABLE `send_result==TERMINAL`을 typed 실패로(ENOENT→NotFound, ESHUTDOWN/ETERM→Terminated). 하나로 뭉개면 원인 상실 | 계약 |
| E2 | close/context 종료 시 대기 토큰·snapshot·native 자원을 한 번만 해제하고 예약 wake/handle 취소 | Node close 뒤 남는 handle |

### 신규 (0.18.0)
| ID | 아이템 | 근거·효과 |
|---|---|---|
| X1 | routed multipart 수신을 단일 native 호출로 materialize(N-API 왕복 5→1) | Node routed 수신 ~2× (commit 28fdce3c10) — 교차 적용 후보 |

## 3. 언어별 적용 매트릭스

범례: **●** 적용·소스 검증  ·  **◐** 적용됐으나 기록 형태와 다름(§4 주석)  ·  **○** 다른 방식으로 대체/구조상 무관  ·  **·** 이 언어엔 기록 없음  ·  **✗** 기록됐으나 소스에 없음

| 아이템 | C | C++ | .NET | Java | Node | Go | Rust | Py |
|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| S1 completion/Promise/map 미등록 | ○ | ● | ● | ● | ● | ● | ● | · |
| S2 poller/goroutine 미생성 | ○ | · | · | · | · | ● | ● | · |
| S3 payload 무복사(거절시점 snapshot) | ● | · | ● | ◐ | ● | ● | ◐ | ● |
| S4 shared submit RwLock | · | · | · | · | · | · | ● | · |
| S5 2-part inline/stack/scratch | · | ● | ● | ● | ● | · | · | · |
| R1 O(1) 키 조회 | · | · | ◐ | · | ● | · | · | ● |
| R2 보류 WRITABLE 재생 | · | · | · | · | · | · | ● | · |
| D1 spin 금지(blocking wake) | ● | ● | ● | ● | ● | ● | ● | ● |
| D2 stray WRITABLE 분리 drain | ● | ● | ○ | ○ | ○ | ○ | ○ | ○ |
| D3 POLLOUT hint 관리 | ● | ● | · | · | · | · | · | · |
| D4 runtime owner lazy/idle 종료 | ○ | ◐ | ○ | ○ | ○ | ● | ● | ○ |
| D5 EINTR≠종료 typed | · | · | ◐ | · | · | · | · | · |
| V1 direct reply | ● | ◐ | ◐ | ◐ | ● | ○ | ◐ | ○ |
| V2 수신 wrapper 재사용·payload 캐시 | · | ● | ● | · | ● | · | · | · |
| V3 소유 msg_t zero-copy 제출 | · | · | · | ◐ | · | · | · | · |
| V4 wrapper storage 초기화 생략 | · | ● | · | · | · | · | · | · |
| E1 TERMINAL typed 매핑 | · | ● | ● | ● | ● | ● | · | · |
| E2 close 1회 해제·handle 취소 | · | ● | · | · | ● | · | · | · |
| X1 routed 단일 native materialize | · | · | · | · | ● | · | · | · |

## 4. 언어별 현황·판정

판정 비율은 별도 표기 없으면 **0.17.2 캠페인, tcp, 6 pattern**(목표는 언어별 상이) 기준이다.

**C** — native baseline(직접 Core API). 소스 검증 6/6 존재. `direct reply`·`stray WRITABLE 스킵`·`DD POLLOUT 스핀 방지`·`전송당 복사 제거`·`STREAM retained 핫패스 제거` 모두 실재. STREAM/retained는 "멤버 삭제"가 아니라 **핫패스에서 제거·backpressure 재전송용으로만 잔존**.

**C++** — 검증 9/12. 통과 4(DD 97.6·PUBSUB 95.3·SS 91.9/91.7), 보류 2(REQREP 72.6/75.8). 드리프트: `V1`은 토큰/routing 재사용으로 구현되고 프레임을 그대로 되쏘는 헬퍼(`submit_received_reply_parts`)는 **dead code**; `D4`는 재진입 "허용"이 아니라 단일소유 직렬화 lease(EBUSY throw); `V4`는 `no_init_t` 전용 생성자 경로. **✗ context term wake는 바인딩에 없음 — Core 책임**(대기자 wake는 close→shutdown condvar이 담당).

**.NET** — 검증 7/11. 보류 6(DD 62.5·PUBSUB 61.0·DR SS 63.9·RR SS 69.5·DR REQREP 60.5·RR REQREP 63.1; 목표 70) — 비용 지도 완료, 남은 지배 항목(send builder 80.5 B/msg, message helper P/Invoke)이 **공개 API 계약**에 묶여 보류. 드리프트: `S2 source-gen import`는 msg/send/recv 핫패스만(나머지 `[DllImport]`); `V1`은 shared-refcount copy(프레임 move 아님); `D5`는 EINTR만 관용(EBUSY는 펌프 미관용, 비-EINTR 실패는 전부 대기자 실패); `R1 HashSet 제거`는 1–2파트만(3+는 유지).

**Java** — 검증 7/9(FFM, JNI 아님). PUBSUB 93.2·DR SS 73.2·RR SS 84.9 통과; DD 72.0·DR REQREP 46.3·RR REQREP 47.6 미달(REQREP 둘 다 65536 B 붕괴 10.9~11.2%). 드리프트: send가 **두 갈래** — raw 경로(`SocketSendPlane`)만 native 핸들 직접 전달, framework 경로(`CompletionOwner`)는 `zlink_msg_copy` refcount 공유. 따라서 `V1`(direct reply)·`V3`(request zero-copy)는 payload는 refcount 공유(zero-copy)지만 native 헤더는 scratch copy 스테이징 — "native move"는 아님. 65536 B REQREP은 request terminal(retained+WRITABLE 회복)과 Core admission 상호작용이 다음 조사 항목.

**Node** — 검증 10/12. 0.17.2 전부 미달(DD 43.1·PUBSUB 29.6·DR SS 33.7·RR SS 35.5·DR REQREP 32.5·RR REQREP 29.7; 목표 60). **0.18.0 진행**: routed 수신 단일 native materialize(`X1`, ~2×) 채택; reqrep/SENDSEND는 러너 completion drain 정합(OK 버스트 중 주기 drain)으로 실패 셀 해소 후 **보류 확정** — reply/echo 수신은 이미 단일 materialize이고, 남은 비용은 public 연산마다 필요한 submit/recv/completion N-API + libuv floor(reqrep tcp64 ~34.6%, SENDSEND ~18.6%). 드리프트: `R1 identity cache`는 키 캐시가 아니라 **바이트 검증 후 재사용**; `E2 close 취소`는 immediate/timer가 아니라 uv_poll/uv_idle teardown + closed 가드. 수신 readiness 공개 API `setReadableHandler`(uv_poll, 1ms 타이머 대체) 확인.

**Go** — 검증 5/5 존재(구현은 `internal/native/`). PUBSUB 39.8·DD 31.5·DR SS 50.1·DR REQREP 67.7·RR REQREP 67.1 미달; RR SS 65536 B 차단(server shutdown). DD 64 B 14%·latency 7x = turn 구조의 제출당 goroutine 비용 후보. `E1`의 NotAdmitted는 미매핑 errno 기본값(설계).

**Rust** — 검증 6/7. DD 60.5·PUBSUB 89.0·DR SS 73.5·RR SS 63.8·DR REQREP 54.3·RR REQREP 55.1 미달(목표 95/85). 드리프트: `V1 try_clone reply`는 API는 있으나 reply 경로 미사용 — `submit_shared_message`의 `zlink_msg_copy` 공유복사로 대체(deep-copy 회피 의도는 유지). `S3`도 poller/entry 할당은 제거했으나 part별 공유복사·native Vec 할당은 잔존.

**Python** — 검증 6/6 존재(순수 py `_runtime/` + native `_native/hotpath.h` 이중구현, 실제 핫패스는 `.so`). DD 15.9·PUBSUB 34.0·DR SS 24.2·RR SS 23.3·DR REQREP 14.2·RR REQREP 15.7 미달(목표 60) — 전 pattern 14~24%로 **평탄**(인터프리터·GIL·asyncio 경계의 고정 per-message 비용). 다음은 "메시지당 Python 함수 호출 수" 축소.

**STREAM (Core 0.17.3, 7언어 × tcp/ws/wss/tls)**: C++·.NET·Java·Go·Rust 통과; Node 35~51%·Python 19~33% 미달(다른 pattern과 같은 고정 비용 성격).

## 5. 기각 후보 (다시 시도하지 않을 것)

**공통 안티패턴**
- public wrapper·Future/Task/CompletionStage·callback userdata의 **pool 재사용**(늦은 completion이 새 operation을 완료시키는 ABA 위험; `GCHandle` 재사용 .NET).
- 실행기 turn마다 **timeout-0 poll로 진행을 흉내내는 spin** — 측정값이 좋아 보여도 CPU 100%이며 다른 스레드 진행을 빼앗는다.
- **native 호출 합치기/배치** — 내부에 이미 queue가 있어 이중 큐·중복 처리로 문제가 커진다(Python multipart outbound를 한 native 호출로 합치기 = allocator corruption). completion을 N건씩 배치 drain하는 후보도 Node에서 무효(−1.1%)로 기각.
- 인위적 **in-flight 상한**·2단계 측정으로 backpressure 우회(PERF_MULTI_TEST_POLICY §1.2/§5.1 위반).

**언어별**
- **C++**: `_continuation_weak` fallback 제거(DR 72.9→69.4 악화), REQUEST completion 중복 합류 제거(악화), REQUEST에 SEND ID 0 즉시완료 적용(request terminal 계약 위반), entry/result 통합, RID snapshot을 빌린 pointer로, route/pipe cache(Core 소유), WRITABLE 즉시 재제출(계약: NO_DATA까지 비운 뒤).
- **.NET**: send builder 80.5 B/msg 제거(공개 operation 객체 자체), close를 다음 init에 합침(ownership release=terminal 계약), 전환 10→9(효과 0), P/Invoke 융합 13→9(−9% 역효과), operation 객체 재사용(one-shot 계약 위반).
- **Node**: direct 2-part native submit; TSFN STREAM payload pool(mutex·반환 비용); **(0.18.0 reqrep/SENDSEND 개선 시도 전부 기각)** native completion batch(−1.1%), per-op closure 제거(−11.3%), completion 종류별 필드 분리(latency 회귀), lazy admission Promise(노이즈), 성공 submit BigInt화(−5.3%), 빈 routed tail 즉시 materialize(+16.3%지만 큐 지연 190ms 폭증), copy+close 단일 take(−4.4%).
- **Python**: multipart를 한 native 호출로 합치기(allocator corruption), 중앙 round-robin scheduler, 측정 tuple 사전 생성, direct `Received` pool(+2.81%뿐), `POLLIN|POLLCOMPLETION` 조합(drain 안정성 저하), GIL 유지 `PyDLL` 호출(교착).
- **러너**: pending 상한으로 relay 누적 막기(답은 "앞 admission을 기다린 뒤 제출"), `send_drain_timeout`·deadline 늘리기, 중앙값이 정상이라고 개별 run 이상 무시.

## 6. 적용 절차

1. 대상 binding `perf/run_benchmarks.sh --pattern DEALER_ROUTER --transports tcp --msg-sizes 1024 --duration 3 --runs 1`로 "전"을 기록하고 같은 조건 C 값을 같은 시각에 잰다.
2. §2 카탈로그를 성공 → 거절 → drain/wake → 수신 순으로 코드에서 판정하고 표로 남긴다. **후보 선정 전에 §5를 확인**해 기각된 방향을 배제한다.
3. 수정마다 public API·ownership contract test와 sleep 없는 회귀 테스트(5회 반복)를 추가한다.
4. "후"를 같은 명령으로 재고, single(PAIR·DEALER_ROUTER·PUBSUB) + multi(clients 8, 1024·65536, DEALER_DEALER·DEALER_ROUTER_SENDSEND·PUBSUB) 스모크가 status complete·0 없음인지 확인한다.
5. 5% 이상 개선(또는 계약 보존 구조 개선)만 §2·§3·§4에 등재하고 근거를 함께 적는다. 러너 정합의 효과는 library 최적화와 합산하지 않는다.
