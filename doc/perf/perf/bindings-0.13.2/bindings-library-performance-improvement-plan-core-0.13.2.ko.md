# Core 0.13.2 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-08-25
>
> 작업 브랜치: `core-0.13.2-bindings-performance` (최신 `main` 병합: `0d12a1e8f5`)
>
> 기준 Core: local worktree의 `VERSION=0.13.2`, `core/build`

이 문서는 `bindings-library-performance-improvement-plan-core-template.ko.md`를 0.13.2의
local Core 측정용으로 새로 시작한 실행 문서다. 0.13.0 문서, report와 판정은 이 문서의
기준값이나 완료 근거로 사용하지 않는다. 원시 반복값과 비교 결과는 같은 폴더의
`bindings-library-performance-core-0.13.2.xlsx` 및 `log/`에 기록한다.

## 1. 기준과 재현 조건

- C 기준과 모든 binding은 현재 branch의 local Core를 사용한다. runner에는
  `ZLINK_CORE_SOURCE=local`을 명시하고 `--core-version`을 사용하지 않는다.
- 각 paired 비교는 같은 Core revision, local runtime 경로, suite, pattern, transport,
  message size, duration, runs, client 수, auto-HWM profile, Effective Options를 사용한다.
- C를 먼저 측정한 직후 같은 조건으로 binding을 측정한다. Core 또는 binding source가
  바뀌면 해당 C 기준부터 다시 측정한다.
- report의 `META,core_source,local`, `META,core_version,0.13.2`, `META,commit`과
  `PERF_CORE_RUNTIME`이 모두 기록되어야 한다. 다른 runtime·release 결과는 참고용일 뿐이다.

## 2. 범위

| 순서 | 언어 | perf 경로 |
|---:|---|---|
| 1 | C++ | `bindings/cpp/perf` |
| 2 | .NET | `bindings/dotnet/perf` |
| 3 | Java | `bindings/java/perf` |
| 4 | Node | `bindings/node/perf` |
| 5 | Go | `bindings/go/perf` |
| 6 | Rust | `bindings/rust/perf` |
| 7 | Python | `bindings/python/perf` |

C reference는 `bindings/c/perf`다. 비율은 같은 paired 결과에서 다음처럼 계산한다.

```text
binding ratio (%) = binding throughput / C throughput * 100
```

Single 기본 size는 `64, 256, 1024, 65536, 131072, 262144` bytes다. Multi는 runner가
등록한 client 수와 size inventory를 report에 그대로 남긴다. runner에 등록되지 않은 pattern은
제외 근거를 log에 기록한다.

## 3. 목표와 판정

각 transport의 throughput gate는 size ratio의 산술평균(aggregate mean)이다. latency gate는
size별 mean latency ratio의 중앙값이다. 개별 size의 최소 기준 미달은 outlier로 기록하되,
aggregate를 통과한 transport를 그 값만으로 실패로 바꾸지 않는다.

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo |
|---|---:|---:|---:|---:|
| C++ | 85% / 95% | 80% / 85% | 75% / 85% | 80% / 85% |
| .NET | 64% / 85% | 75% / 80% | 50% / 70% | 50% / 70% |
| Java | 70% / 90% | 75% / 85% | 50% / 70% | 50% / 70% |
| Node | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |
| Go | 55% / 65% | 50% / 57% | 40% / 53% | 40% / 53% |
| Rust | 85% / 95% | 70% / 85% | 70% / 85% | 70% / 85% |
| Python | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |

왼쪽은 개별 size 최소 기준, 오른쪽은 transport aggregate 목표다. C++ 단순 one-way는
개선 pass가 과도하게 길어질 때만 90% 완화 목표를 선택할 수 있으며 근거를 log에 기록한다.
C++/Rust의 latency 상한은 2.0x, .NET/Java/Go는 3.0x, Node/Python은 5.0x다.
secure transport(`tls`, `wss`)와 목표 경계 ±5%p는 C→binding 모두 5회 중앙값으로 최종
판정한다.

상태는 다음 네 가지뿐이다.

- `미측정`: 유효한 paired C/binding report가 없다.
- `통과(비율%)`: aggregate throughput·latency, 회귀, options, auto-HWM, client 수 조건을
  충족한다.
- `미달(비율%)`: aggregate throughput 또는 latency가 목표에 미치지 못했다. log에 개선 pass
  상태와 후보 결과를 반드시 기록하며, 개선 pass가 진행 중인 동안에는 다음 셀로 이동하지 않는다.
- `보류(미달 비율%)`: 유효한 paired 재측정, 계약 회귀, 구현·측정된 후보와 **Sol reviewer의 후보
  소진 결론**까지 모두 log·시트에 남겼지만 목표에는 미달한 최종 상태다. 이때만 다음 셀로 이동한다.

`보류`는 단순한 주관적 판단이나 no-go 하나만으로 지정하지 않는다. Sol review 전에는 상태를
`미달`로 유지한다.

## 4. 강제 작업 순서

1. local Core와 대상 binding을 build하고 단일 64B smoke를 C→binding 순서로 실행한다.
2. 같은 manifest에서 모든 size를 C로 측정한 직후 binding before를 측정한다.
3. aggregate가 미달이면 **다음 transport·pattern·언어로 이동하지 않는다**.
4. profiler·allocation·copy·callback/dispatch·native boundary를 확인하고, public hot path의
   후보 A를 구현한다. 후보마다 POSDDD 책임 경계, 중복·불필요한 상태/할당/분기 제거 이득도
   별도로 평가한다. 성능 A/B의 조건을 바꾸지 않도록 리팩토링은 후보와 같은 작은 경계에
   한정하고, before/after와 기능 회귀를 기록한다.
5. 후보 A가 목표를 못 맞추거나 no-go이면 read-only Sol review를 받고, 공개 contract를
   유지하는 후보 B를 구현·측정하거나 no-go 근거를 남긴다.
6. 구현·측정 가능한 후보를 끝낸 뒤 Sol reviewer가 안전한 후보 소진을 결론 내리면, clean source의
   동일 manifest C→binding final pair를 다시 측정한다. 목표를 못 맞추면 `보류(미달)`로 확정하고
   C report, binding report, 후보 결과, Sol 의견, contract 회귀 결과를 log와 시트에 남긴 뒤에만
   다음 항목으로 이동한다.
7. 개선을 채택한 경우에만 source·검증·paired report·문서를 한 커밋으로 만들고 push한다.

허용되는 변경은 public interface, message/routing-id ownership, 정확히 한 번 완료, close와
failure semantics, cancellation, concurrency, callback context의 계약을 보존해야 한다. C API나
private/native handle 우회, public ownership 변경, timeout/close 동작 변경, mutex·weak reference
제거, pool 확장, large-message pool 확대는 금지한다. POSDDD 개선만으로 성능 미달을 통과로
바꾸지 않는다.

각 후보는 `doc/principal/dev/posddd.ko.md`의 POSDDD 원칙을 유지해야 한다. 처리량 개선과
별개로 정보 은닉·책임 경계·중복 제거·불필요한 상태/할당/분기 제거 이득을 평가해 기록한다.
성능 회귀, 새 복잡성, contract 위험이 있으면 그 정리는 남기지 않는다. POSDDD 이득만 있는
후보는 채택할 수 있지만 performance 상태를 `미달`에서 `통과`로 바꾸지 않으며, 성능 수치와
POSDDD 채택 근거를 같은 log·시트에 남긴다.

## 5. C++ Single 측정 표

모든 셀은 새 local Core 기준의 `미측정`으로 시작한다.

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | Aggregate / report / log |
|---|---|---|---|---|---|---|---|---|
| tcp | PAIR | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tcp | PUBSUB | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tcp | DEALER_DEALER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tcp | DEALER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tcp | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tcp | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tcp | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ws | PAIR | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ws | PUBSUB | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ws | DEALER_DEALER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ws | DEALER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ws | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ws | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ws | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| wss | PAIR | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| wss | PUBSUB | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| wss | DEALER_DEALER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| wss | DEALER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| wss | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| wss | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| wss | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | PAIR | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | PUBSUB | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | DEALER_DEALER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | DEALER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | PAIR | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | PUBSUB | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | DEALER_DEALER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | DEALER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | PAIR | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | PUBSUB | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | DEALER_DEALER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | DEALER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |

## 5-1. .NET Multi 측정 표

local Core 0.13.2, TCP, 100 clients, server/client I/O threads 4/4, 2초×3회 median의
공식 paired C→.NET 결과다. 표의 각 size는 `.NET throughput / C throughput` 비율이다.

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | Aggregate / report / log |
|---|---|---|---|---|---|---|---|---|
| tcp | ROUTER_ROUTER_SENDSEND | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tcp | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |

나머지 .NET Multi pattern/transport는 유효한 paired final report가 생기기 전까지 `미측정`이다.

## 6. 다음 작업

1. C++ Single·Multi와 .NET Multi의 공식 paired 결과 및 문서 내 측정 log 참조를 초기화했다. local Core 기준으로 C를 먼저 측정한 뒤 같은 manifest에서 각 binding을 측정해 모든 항목을 다시 판정한다.
2. Core Router 경로를 POSDDD 기준으로 다시 검토했다. completion TLS 상태를 callback scope에
    응집하고, 1,065줄의 send-completion 구현을 queue/dispatch와 public submit/cancel 두 모듈로
    분리했다. 단일 메시지 pipe fast path는 깨끗한 multipart 상태를 명시적 불변식으로 삼아
    불필요한 누적 산술·초기화 store를 제거했다. 검증 중 소켓 전체 `pending_msgs`가 한 대상의
    HWM을 다른 대상까지 막는 exact-target 결함을 재현해, 대상별 inline reservation과 물리 제출
    gate의 책임을 분리했다. 관련 CTest 6/6, C/C++ focused Multi 각각 4/4가 통과했다. 전체
    CTest의 유일한 실패였던 삭제된 threading 문서 경로 참조도 현재 guide 경로로 고쳐 재검증했다. 처리량은
    크기별로 혼재해 micro 최적화의 일관된 향상을 주장하지 않지만, exact-target head-of-line
    차단 제거와 구조 응집 효과가 있어 변경을 채택한다. auto-HWM 하향 후보는 일부 개선이
    보였으나 동일 1MiB 조건에서도 큰 run drift가 관찰되어 정책 변경 근거가 부족하므로 보류했다.
    상세 근거는
    [`log/core-router-posddd-review-20260826.ko.md`](log/core-router-posddd-review-20260826.ko.md)에
    기록했다.
3. Core 전체 Multi 7패턴의 TCP/WSS 경로를 POSDDD 기준으로 검토하고 리팩토링했다. WSS engine이
    종료 시 weak-guard 만료로 취소 callback을 버리면서 pending flag를 영구 유지하던 수명주기 결함을
    수정하고, handshake/read/write 완료까지 engine이 명시적으로 생존하게 했다. TCP/WS/WSS
    completion은 copy 대신 move ownership으로 전달하고, ROUTER exact-target의 RID 중복 조회를
    제거했으며, STREAM buffer/message dispatch send의 중복 정책을 하나의 도메인 함수로 응집했다.
    기존 WSS 기준선은 기본 3초 cooldown에서도 allocator 손상으로 partial이었지만 수정 후 secure
    5-run 전체 42/42가 완주했다. TCP도 3-run 전체 42/42가 완주했고 7패턴 중 6패턴의 before 대비
    평균 처리량이 103.92%~122.76%였다. PUBSUB 전체 run의 90.91%는 즉시 재측정에서 104.68%로
    반전되어 run drift로 판정했다. WSS 유효 셀 비교는 ROUTER_ROUTER_SENDSEND 97.48%와
    DEALER_DEALER 97.19%를 제외하면 103.54%~119.98%이며, 핵심 판정은 실패 0의 lifecycle
    안정성이다. exact-target detach/admission 경합 테스트는 실제 completion 유실이 아니라
    ADMITTED가 detach보다 먼저 확정된 합법 결과였으므로 operation id별 exactly-once를 검증하게
    고치고 한·영 spec에 두 합법 결과를 명시했다. ASAN/LSAN WS/WSS 13/13, STREAM 경합 10회,
    전체 Core CTest 103/103이 통과했다. 상세 근거는
    [`log/core-all-pattern-posddd-tcp-wss-20260826.ko.md`](log/core-all-pattern-posddd-tcp-wss-20260826.ko.md)에
    기록했다.

## 7. 완료 기준

- 대상 언어의 Single과 Multi runner inventory가 이 문서와 시트에 일치한다.
- 모든 최종 report는 local Core 0.13.2와 같은 manifest의 paired C/binding `status: complete`다.
- 모든 `미달`은 자체 개선 pass, Sol pass, candidate after 또는 no-go, contract/기능 회귀
  결과를 갖는다.
- report가 없는 셀은 `미측정`으로 남기며 완료를 주장하지 않는다.
- 개선 채택은 source, 검증, report, 문서가 같은 commit으로 push된 경우만 유효하다.
