# A 인계 — DONTWAIT send 계약 변경 (0.17.0, 판정 D-B79)

작성: 머신 B, 2026-09-04 15:10 / 개정 16:00 (계약 B로 확정) / 개정 18:40 (Core·바인딩 커밋, 인계) / **개정 2026-09-05 05:45 (REQUEST 통일 완료, perf 정책 복원 완료, 최종)**.
상태: Core 커밋 `50d77800f2`(main), 스펙 갱신 커밋(이 문서와 같은 커밋), 바인딩 cpp/node/dotnet 커밋 `4f503b76d3`, java 커밋 `7927c582c2`,
0.17.0 범프 커밋 `70a9998998`, c `f9d0eb84d9`, rust `85eb9425a1`, python `5240947587`, go `4ff46b5bae`.
리뷰(버그·핫패스·스모크) 커밋: c `fc0562cef4`, rust `9f2342cf27`, dotnet `8b52bb66ba`, cpp `5a42a363c7`, go `c6fdad2194`,
python `cd5b4a163e`, node `a9eb6c5a77`, java `b72622bb2d`. 리뷰 결과 표는 §6.
**REQUEST도 같은 계약으로 통일(D-B85, 2026-09-05)**: Core `7d8205a028`, 스펙 `ea934d0e97`, bindings dotnet `2099bb045a`(병합), java `a06260f507`,
node `b145f86501`, cpp `e0860723bc`, python `78eed9ce96`, rust `9728a0e081`, go `7afa27e72b`, c 계약 테스트 `ba615d3137`(C perf REQREP 러너 `0f9c329764`).
Core 후속: SUB session use-after-free 수정 `29add0ac81`, 단일 파트 REPLY 64 KiB 수정 `5e26e72806`, 토큰 경로 리팩토링 `900ea8319e`(hotpath ±0.04%).
C++ hot-path pass 1·2 `86b897abf7`·`e6dd88fbc6`(공개 API 불변). 계약 요약은 §7. 계약의 최종 문장은 `core/doc/spec/core/socket/README.ko.md`
"Part send" 절이며 아래 표는 요약이다.

## 1. 바뀌는 계약 (사용자 확정, B안)

| 상황 | 0.16.0 현행 (bb66e85376) | 0.17.0 (신규, B) |
|---|---|---|
| DONTWAIT FINAL, 즉시 admission 가능 | `ZLINK_SUBMIT_OK`, ID 0, completion 없음 | 동일 |
| DONTWAIT FINAL, 대상 pipe HWM/byte credit 부족 또는 대상 미준비 | pending 수락(payload 보관, 기본 무제한), 나중에 SEND completion | **`ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, completion ID = nonzero 대기 토큰, `user_context` 기억. payload는 호출자 소유(Core는 토큰·대상 pipe·context만 보관)** |
| "다시 보내도 됨" 신호 | (pending이 알아서 전송) | 대상 pipe에 credit이 돌아오면 Core가 completion queue에 **`WRITABLE` completion**(토큰·context·가능하면 RID)을 넣고 poller를 **`POLLOUT`**으로 깨움(POLLCOMPLETION도 level 성립) |
| 앱 동작 | completion을 기다림 | POLLOUT → `zlink_completion_recv`를 NO_DATA까지 pull → WRITABLE의 context로 **정확히 그 코루틴**을 재개 → 같은 패킷 재전송 |
| 대상 단위 | — | PAIR=단일 pipe, DEALER=후보 집합(어느 후보든 열리면 WRITABLE 1건, 재전송 시 Core가 열린 peer 선택), ROUTER/STREAM=지정 RID pipe |
| 토큰 수명 | — | (a) WRITABLE 전달, (b) 대상의 명시적 제거(`zlink_disconnect_rid`, 해당 RID endpoint 종료) → WRITABLE + `ZLINK_SEND_TERMINAL`/`ENOENT`, (c) socket close·context termination → WRITABLE + `ZLINK_SEND_TERMINAL` + lifecycle errno. peer weight 0은 SEND 토큰을 끝내지 않음 |
| ROUTER/STREAM에서 route가 없는 RID | `NOT_CONNECTED` | **동일: 즉시 `ZLINK_SUBMIT_NOT_CONNECTED`(EHOSTUNREACH), 토큰 없음**(ROUTER는 MANDATORY 양수일 때, 기본값; 0이면 기존대로 조용히 버림). route는 있으나 미준비(pair 미준비·weight 0·HWM)면 토큰. DEALER peer 0개는 토큰 |
| reservation(65,536/socket, REQUEST와 공유) 소진 | — | SEND `ZLINK_SUBMIT_OUT_OF_MEMORY`/`ENOMEM`, ID 0 (REQUEST는 현행 BACKPRESSURED/EAGAIN) |
| completion kind | REQUEST, SEND | REQUEST, **WRITABLE**(신규 enum 값, 기존 값 불변); 일반 SEND 성공에는 completion 없음 |
| `ZLINK_OPT_PENDING_MAX_MSGS/BYTES` | SEND·REQUEST 공유(기본 무제한) | SEND에는 no-op(REQUEST 현행). enum 값·ABI 유지 |
| blocking `NONE` send / `zlink_request_part` | 현행 | 현행 유지 |

핵심: HWM은 다시 흐름 제어 신호다. Core는 패킷을 보관하지 않고 "누구를 깨울지"만 정확히 알려준다(대상 단위). 콜백은 재도입하지 않는다(스레드 경계).

## 2. framework가 바꿔야 할 것

- DONTWAIT send가 `BACKPRESSURED`이면 반환된 completion ID(토큰)와 함께 그 코루틴을 **토큰/context 키로** 대기 목록에 넣고 suspend(패킷은 코루틴이 보유).
- framework의 poller 루프(POLLIN·POLLCOMPLETION 처리하는 그 루프)가 `POLLOUT`을 받으면 completion queue를 NO_DATA까지 pull. `WRITABLE` completion의 context로 해당 코루틴만 재개 → 같은 패킷 재전송(다시 BACKPRESSURED면 새 토큰으로 다시 대기). REQUEST completion 처리는 그대로.
- 0.16.0의 "nonzero ID = pending 수락, 나중에 SEND completion = 전송 완료" 가정 제거(이제 nonzero ID = 대기 토큰, WRITABLE = 재시도 신호).
- 같은 대상의 전송 순서가 필요하면 framework 논리 큐로 보존(Core는 순서 큐를 갖지 않음).
- `ZLINK_OPT_PENDING_MAX_*`를 SEND 목적으로 쓰는 코드 제거(REQUEST용 유지).
- 현재 EAGAIN/POLLOUT/BACKPRESSURED 처리 코드: cpp 6파일, dotnet 32, java 15, **node 0**(`rg -n 'BACKPRESSURED|POLLOUT|EAGAIN' framework/languages/<lang>`).
- WRITABLE record의 `send_result == ZLINK_SEND_TERMINAL`(errno ENOENT/ESHUTDOWN/ETERM)은 "재전송 불가"이므로 해당 코루틴을 실패로 깨울 것(대상 제거·close). 바인딩은 이를 typed 실패로 노출한다(cpp/node/dotnet 커밋 `4f503b76d3` 메시지 참고).
- 바인딩 public API 변경 요약(각 바인딩 README 갱신됨): `CompletionKind.WRITABLE` 추가(SEND는 ABI 유지·미발행); async send는 즉시 성공(ID 0) 또는 토큰 대기 후 같은 payload를 바인딩이 재전송(framework가 payload를 다시 만들 필요 없음); completion queue는 public poller 하나가 단독으로 drain; PENDING_MAX 옵션은 REQUEST 전용.
- dotnet 한정: Core에 waitable poller FD가 없어 poller 미등록 상태의 autonomous await는 nonblocking scheduler turn으로 진행한다(public poller를 등록하면 그 루프가 진행을 구동). 완전한 무스핀 대기가 필요하면 Core ABI 확장 항목으로 별도 논의.

## 3. 검증 방법(framework 쪽)

- 테스트 시나리오: HWM까지 채움 → DONTWAIT가 BACKPRESSURED → peer drain → POLLOUT → 재전송 성공, 데이터 순서 유지.
  sleep 기반 대기 금지, 5회 반복 green.
- Core 회귀 테스트(B가 추가): `core/tests/integration/test_wake_invariants.cpp`의 HWM EAGAIN→POLLOUT 케이스와
  100 client × 64 KiB 케이스, 신규 DONTWAIT 거절 케이스.

## 4. 버전·문서

- Core·bindings 버전 0.17.0(사용자 결정). B가 Core 커밋 뒤 `VERSION`/`BINDINGS_VERSION` 범프 + `sync-version.py`
  + 수동 매니페스트(python/rust/java/node/go allowlist)까지 처리.
- 스펙 정정(B, 별도 커밋, ko/en): `core/doc/spec/core/socket/README.ko.md` 116·375~389·923~955행, `06-dealer` 100·289~290·323~327행,
  기타 socket 문서 DONTWAIT 문장, `05-polling`에 POLLOUT 관측 명시, `doc/perf/PERF_MULTI_TEST_POLICY.md §1.2` pull 대응 문단.
- bindings(c/cpp/go/rust/python/node/java/dotnet) API 정합은 B가 언어별 job으로 처리(배포 제외).

## 5. 진행 상태

- [x] A안(POLLOUT만) 구현 중 계약 B로 확정 → A안 job 중단, 공통 부분(payload 보관 제거·REQUEST 분리) 유지
- [x] Core 수정 B — 커밋 `50d77800f2` (리뷰 후속 3건 포함: route 없는 RID는 NOT_CONNECTED, 내부 훅 제거, 명시적 제거 시 토큰 TERMINAL/ENOENT; D-B80)
- [x] B gate — dev ctest 139/139, release-gate(LTO) hotpath_gate PASS, 변경 suite 5회 green, cpp/node/dotnet 바인딩 green. 벤치 before/after 표는 perf multi 정책 복원 job 뒤(미완)
- [x] 스펙 정정 커밋(ko/en 14파일) — 이 문서와 같은 커밋
- [x] bindings cpp/node/dotnet — 커밋 `4f503b76d3`
- [x] bindings java — 커밋 `7927c582c2` (LINGER 옵션 ID 라우팅 버그, REQUEST 경로 직렬화 제거)
- [x] 0.17.0 범프 커밋 `70a9998998`
- [x] bindings c/go/rust/python 포팅 — 위 해시
- [x] bindings 8개 독립 리뷰(계약 a–h, 핫패스, 테스트·샘플·perf single/multi 스모크) — §6 (java는 후속 커밋)
- [x] perf multi 정책 복원(§1.2 모델 + WRITABLE drain) + before/after 표 — C 러너 SEND 토큰 모델·REQREP 토큰 모델(`e5770ec569`, `0f9c329764`), ws/wss REQREP 제출 턴 수정 `21746768ca`(D-B89); Core 0.17.0 vs 0.15.1 판정 D-B87/D-B88(`doc/plan/core-0.17.0-dontwait-contract-and-perf-plan-b.ko.md` §2)
- [x] REQUEST 계약 통일(D-B85) — 8 bindings 모두 커밋(위 해시), Core `7d8205a028`, 스펙 `ea934d0e97`
- [ ] bindings 0.17.0 성능 계획 실행(C++ 진행 중, 이후 .NET → Java → Node → Go → Rust → Python) — `doc/perf/perf/bindings-0.17.0/bindings-library-performance-improvement-plan-core-0.17.0.ko.md` §9·§11. A에는 영향 없음(공개 API 불변, hot-path만)

## 6. 바인딩 리뷰 결과 (2026-09-04 저녁, 각 바인딩 독립 리뷰 job)

모든 바인딩에서 계약 항목 (a)~(h)를 코드로 판정했고, 표준 테스트·샘플·perf single/multi 스모크가 green이다. 수치는 DEALER_ROUTER tcp 1024B, duration 3s, runs 1(다른 job과 병행 측정이라 절대값은 참고용).

| 바인딩 | 주요 수정 | 성능(수정 전 → 후) |
|---|---|---|
| c | REQREP 러너의 stray WRITABLE 처리, DEALER_DEALER POLLOUT spin 방지, 전송당 payload 복사 제거 | 442k → 444k (핫패스 불변) |
| cpp | TERMINAL/native -1 typed 매핑, poller 재진입 UAF·실패 보존, close 순서, context term hang, POLLOUT 빈 drain spin 제거 | 697k → 775k |
| dotnet | payload snapshot을 거절 시점으로, pump spin 제거, ownership rollback, EINTR 과잉 실패 정정, ESHUTDOWN 매핑 | 267k/297k ↔ 281k/287k (동률) |
| node | 성공 경로의 entry/Promise/map 등록 제거, zero-timeout pump spin 제거, ESHUTDOWN 매핑 | 28k(포팅 후) → 145k (포팅 전 153k) |
| java | (후속 커밋) runtime owner spin 제거, terminal typed 매핑, 즉시 성공 경로 할당 제거 | — |
| go | 즉시 성공 send의 poller/goroutine 생성 제거, drain spin 제거, terminal 원인 보존, multi 러너 종료 hang | 81.5k → 190k |
| rust | 실행기 spin 루프 → reactor 스레드 + public Poller 구동, 전송당 poller/복사/할당 제거, REQUEST spin 제거 | 172k(포팅 전) → 259k |
| python | 성공 경로 bytes 이중 복사 제거, event loop spin 제거, O(n) 해제 → O(1), multi 러너 PUBSUB STOP·timeout 판정 | 7.9k → 20k |

framework에서 확인할 점: WRITABLE `send_result == TERMINAL`(ENOENT → NotFound, ESHUTDOWN/ETERM → Terminated)은 각 바인딩이 typed 실패로 노출한다. ROUTER의 route 없는 RID는 MANDATORY가 양수(기본)일 때만 NOT_CONNECTED이고 0이면 기존대로 조용히 버린다.

## 7. REQUEST 계약 통일 (D-B85)

`zlink_request_part` DONTWAIT FINAL도 SEND와 같은 모델이다.

| 상황 | 결과 |
|---|---|
| 즉시 admission | `ZLINK_SUBMIT_OK` + nonzero REQUEST ID(현행), reply/timeout completion. Reply timeout은 admission 시점부터 |
| HWM/credit 부족, 대상 미준비(pair 미준비·weight 0·DEALER peer 0개) | `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`+**대기 토큰**(payload는 호출자 보유) → credit 회복 시 `ZLINK_COMPLETION_WRITABLE`(같은 토큰·context·RID) → 같은 요청 재제출 |
| ROUTER route 없음(MANDATORY 양수) | `ZLINK_SUBMIT_NOT_CONNECTED`, 토큰 없음 |
| 토큰 종료 | WRITABLE / 명시적 제거(TERMINAL+ENOENT) / close·term(TERMINAL) |
| `ZLINK_OPT_PENDING_MAX_*` | ABI·저장만 유지, 완전 no-op. Core REQUEST pending pool·"admission 전 pending 시간" 없음 |
| blocking NONE request, reply 제출, PUB publish | 변경 없음 |

framework 영향: 0.16.0의 "nonzero REQUEST ID = admission 전 pending 수락" 가정 제거. 바인딩의 async request는 BACKPRESSURED+토큰을 SEND와 같은
토큰 기계로 처리하므로(자기 토큰의 WRITABLE에서만 같은 요청 재제출, admission 뒤 기존 REQUEST completion), framework는 바인딩 request API를
그대로 쓰면 되고 직접 재시도 루프를 두지 않는다. TERMINAL WRITABLE은 typed 실패(NotFound / Terminated)로 온다.

## 8. A 후속 6건 결과 (2026-09-05 오전, `handoff-B-followups-D086-D087.ko.md` 요청)

| # | 작업 | 결론 (decisions) | 커밋 |
|---|---|---|---|
| 3 | reciprocal HANDOVER lane | spec대로 — 패배 lane은 standby 유지, 자동 종료 없음. settlement/re-pin 코드 삭제 가능 (D-B96, `test_router_reciprocal_handover_lanes`) | `a1cbfb3246` |
| 4 | disconnect progress w/o app poll | Core 버그 3건 수정 — 두 번째 POLLIN poller·수동 reconnect 상태기계 삭제 가능 (D-B104, `test_socket_disconnect_progress_without_app_poll`) | `0c39ed2e52` |
| 5 | Java monitor identity | Java 레이아웃 버그 수정 + Core identity 결함 3건 수정(inproc id·tcp/ipc attempt id·inproc 재연결) (D-B95, D-B102); tcp CLOSED correlation은 READY==DISCONNECTED, 새 attempt==CLOSED | `c9d294c44f`, `1c69086a4a`, `711fe8a1e3` |
| 6 | Java typed error | spec대로 4 case 구분 + disconnectRid/WRITABLE race 수정 — errno 표·`NOT_ADMITTED` 전체 허용 삭제 가능 (D-B97) | `76596423ff` |
| 1 | D-086 admission 지연 | Core 버그(accepted pair ID 재사용) 수정, tcp 4~7 ms; 2 s 기대치 조정 불필요 (D-B94) | `7ffb8e55d9` |
| 2 | D-087 Java 누수 | 콘텐츠 해시 캐시 (D-B93) | `37af8073a7` |

패키지 재빌드 대상 Core 커밋: `7ffb8e55d9`, `1c69086a4a`, `0c39ed2e52`. Release+LTO hotpath_gate는 `0c39ed2e52` 기준으로 실행해 결과를 D-B104에 추가한다.
사용자 결정이 필요한 spec gap: (a) inproc peer close 시 CLOSED 이벤트 요구 여부(D-B102), (b) 즉시 `disconnect→connect` 시 old/new overlap 허용 여부·request가 쓴 connection_id 공개 여부(D-B104), (c) Java Poller monitor 등록은 공통 spec 개정(`c6d491e3e8`)으로 해소.
bindings parity(사용자 지시 "모든 bindings 동일"): spec `c6d491e3e8`; Rust `NOT_ADMITTED` 수정 `bd84afc447`; 레이아웃 테스트 `90ad19f0e5`; Poller monitor source — Node `31139dd137`, Java `820b878567`, Rust `ade06d5514`, Python/Go `3c64aeb481` (D-B98~D-B103).
