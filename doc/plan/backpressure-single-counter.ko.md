# 백프레셔 단일 counter 전환 — 작업 계획

작성 2026-09-12. `#280`~`#283`의 상위 계획이다. `#259`와 `#60`은 이 작업으로 대체되어 닫았다.

## 1. 무엇을 하는가

과부하를 **백프레셔 하나로만** 제어한다. 상한을 정해 두고 넘치면 거절하는 방식을 제품 전체에서
없앤다.

세는 곳은 둘뿐이다.

| 세는 것 | 무엇을 재나 | 누가 |
|---|---|---|
| Core byte HWM | queue가 보유한 frame byte | Core |
| `MaxQueuedApplicationJobs` | host가 handler를 기다리는 job **개수** | Framework |

동작은 이렇다.

```
recv 하기 전에 permit을 얻는다   ← 자리가 없으면 recv를 안 한다
job이 자기 handler로 간다
그 handler가 시작되기 직전에 permit을 돌려준다

쓰는 permit이 80 %      → 요청이 들어오는 socket에 PAUSED
60 % 이하               → RUNNING
100 %                   → 더 받지 않는다 → Core queue에 쌓임 → 보내는 쪽이 byte 상한에서 막힘
```

**아무것도 버리거나 거절하지 않는다.** 받은 것은 전부 handler로 간다. 보내기는 Core socket의
send·request를 한 번 부르는 것이고, 기다림은 Core와 binding이 맡는다. 시간이 다 되면
`DeadlineExceeded` 하나로 끝난다.

## 2. 없애는 것

| 상한 | 어디 |
|---|---|
| completion 자리 4,096 | operation registry, completion dispatcher |
| application lane 1,024건·64 MiB | 실행 대기열 |
| lifecycle lane 128건·4 MiB | 실행 대기열 |
| mailbox 건수·byte (`MailboxMessageBudget`, `MailboxByteBudget`) | owner mailbox, 공개 설정 |
| worker 대기열 (`MaxQueueLength`), bounded I/O admission | worker pool |
| 답을 보관할 자리, control claim 한도 | Spot·Actor |
| StreamConnector 수신 큐 폐기 (`ReceivedMessageDropped`) | connector |
| `ErrorKind`의 `CapacityExceeded`(값 6) | 12개로 줄고 값이 0..11로 당겨진다 |

발생 지점은 이렇게 옮긴다.

| 옛 발생 | 새 결과 |
|---|---|
| 줄·표·자리가 찼다 | 오류가 아니다. 기다린다. 시간이 다 되면 `DeadlineExceeded` |
| Spot을 둘 node가 없다 | `Unavailable` |
| HTTP 응답 body 크기 초과 | `Rejected` |

## 3. 남기는 것 — 줄 길이가 아닌 것들

| | 왜 남나 |
|---|---|
| Node별 Actor·Spot limit, stable type별 limit | "이 node에 Spot을 몇 개 둘까". 기다려도 안 생기므로 `Unavailable` |
| `MaxMessageSize` | 메시지 **한 개**의 크기 guard |
| Pending activation 128 | 동시에 진행 중인 activation 수 |
| owner 점유 10 ms, lifecycle 연속 8 turn | 용량이 아니라 **공정성** |
| relocation의 in-flight payload 예산 | host relocation 시 **부하 분산**. 큰 chunk가 같은 연결의 일반 message 대역폭을 독점하지 않게 한다. 백프레셔와 기준이 다르므로 엮지 않는다 |
| Classic fanout(PUB/SUB)의 손실 | 원래 손실 전달. 구독자 하나 때문에 발행자를 멈추지 않는다 |
| topology reason enum의 `capacity_exceeded` | `ErrorKind`와 다른 enum |
| Core의 socket당 completion 예약 65,536 | Core가 소유. 포화는 `BACKPRESSURED`이고 binding이 받아 기다린다 |

## 4. 진행 상태

### 4.1 스펙 (감독자가 직접)

| 단계 | 상태 |
|---|---|
| 1차 — 포화를 거절에서 대기로, `CapacityExceeded` 삭제 | `889e328856` 커밋·푸시 |
| 2차 — 실행 lane·mailbox·worker 상한 삭제 | `671731575b` 커밋·푸시 |
| `sol` 리뷰 12건 반영 | **진행 중** (아래 §4.2) |

### 4.2 리뷰 지적 판정

| # | 내용 | 판정 | 상태 |
|---|---|---|---|
| 2.1 | 실행 lane·control claim이 자체 상한·거절을 가짐 | 채택 | 완료 |
| 2.2 | mailbox budget이 공개 API에 남음 | 채택 | 완료 |
| 2.3 | worker `MaxQueueLength`·bounded I/O admission | 채택 | 완료 |
| 2.4 | Core completion 65,536 삭제 요구 | **기각** — Core 소유, binding이 흡수 | — |
| 2.5 | completion lane HWM-free 삭제 요구 | **기각** — Core 스펙 변경 | — |
| 2.6(a) | relocation hold가 permit을 안 씀 | 채택 | 완료 |
| 2.6(b) | relocation byte 예산 삭제 요구 | **기각** — 부하 분산 목적 | — |
| 2.7 | StreamConnector의 receive·observer·dispatch·outbound 한도 | 판정 필요 | **남음** |
| 2.8 | capacity로 drop하는 publish·fanout·telemetry | 일부 채택 | publish 완료, fanout 기각, telemetry **남음** |
| 2.9 | ko/en 불일치 2곳 | 채택 | 완료 |
| 2.10 | 깨진 anchor 1개 | 채택 | 완료 |

현재 42개 파일 변경(+114/−243), `verify-framework-doc-contracts.sh` `exit=0`.

### 4.3 남은 스펙 작업

1. **2.7 판정** — StreamConnector의 네 한도. 수신 큐는 이미 "읽기를 멈춘다"로 고쳤고, observer
   notification 큐는 **관측**이라 application job이 아니다. 관측을 합치는 것과 일을 버리는 것이
   다른지 확인해 판정한다.
2. **2.8의 telemetry** — `01-runtime-monitoring`의 status 보관 상한과 trace 폐기. 위와 같은
   기준으로 판정한다.
3. 확정 뒤 `sol` 리뷰를 한 번 더 돌려 잔재를 확인한다.
4. 커밋·푸시.

## 5. 구현 (codex job)

스펙 확정 뒤 투입한다. 언어마다 Issue 하나·worktree 하나·브랜치 하나다.

| Issue | 언어 | worktree | 브랜치 |
|---|---|---|---|
| `#277` | Node binding | `zlink-277-node` | `bindings-node/277-reqrep-drain` |
| `#280` | C++ | `zlink-280-cpp` | `framework-cpp/280-capacity-to-wait` |
| `#281` | Java | `zlink-281-java` | `framework-java/281-capacity-to-wait` |
| `#282` | .NET | `zlink-282-dotnet` | `framework-dotnet/282-capacity-to-wait` |
| `#283` | Node | `zlink-283-node` | `framework-node/283-capacity-to-wait` |

지시서는 `doc/plan/issue-259-worklog/briefs/<lang>.prompt`, 언어별 조사는 같은 디렉터리의
`gap-survey.ko.md`다. `#277`은 `zlink-277-node/BRIEF-277.md`를 쓰며 다른 job과 디렉터리가
겹치지 않으므로 함께 돌린다 — `#101`을 막고 있어 먼저 풀어야 한다.

### 5.1 한 번 실패한 이유

1차 투입에서 네 job이 상한 필드를 전부 지웠는데, 그때 감독자가 "상한은 남긴다"로 잘못 판정해
195개 파일을 버렸다. **job들이 맞았다.** 원인은 두 가지다.

- 스펙 2차 수정이 안 끝나 `09-object-lifecycle`에 "상한이 없는 실행 대기열을 두지 않는다"가
  남아 있었다. 감독자가 그 문장을 근거로 판정했다.
- 지시서가 "포화 경로를 대기로 바꾼다"라고만 쓰고 상한을 어떻게 할지 안 썼다.

또 네 job 모두 빌드·테스트를 못 돌렸다. worktree에 `VCPKG_ROOT`·`ZLINK_LOCAL_PACKAGE_ROOT`가
없고 `node_modules`가 없었는데 감독자가 문서의 준비 절차를 안 밟았다.

### 5.2 이번에 준비한 것

| 항목 | 상태 |
|---|---|
| vcpkg | `~/.cache/zlink/vcpkg`에 bootstrap. worktree 4개 공유 |
| `ZLINK_LOCAL_PACKAGE_ROOT` | `/home/hep7/project/zlink/.artifacts/wsl` |
| JDK 25 | `/usr/lib/jvm/jdk-25.0.4.1+1` |
| Node 의존성 | `zlink-283-node`에 설치 완료 |
| 지시서 | 지울 상한 목록, 남길 것 목록, 빌드 절차를 모두 포함 |

### 5.3 진행 (2026-09-12)

| Issue | 구현 | 빌드·테스트 | PR |
|---|---|---|---|
| `#281` Java | 완료 | **1,453개 통과** | [#287](https://github.com/zlink-systems/zlink/pull/287) |
| `#283` Node | 완료 | **전체 통과** | [#288](https://github.com/zlink-systems/zlink/pull/288) |
| `#277` bindings Node | 완료 | 계약 test 2건 통과, 실측 진행 중 | [#289](https://github.com/zlink-systems/zlink/pull/289) |
| `#282` .NET | 완료 | 빌드 0 error, 전체 test 실행 중 | — |
| `#280` C++ | codex 진행 중 (55 파일) | — | — |

`#282`는 `ContractSurfaceCoverage`가 "스펙에서 지운 `MailboxMessageBudget`이 소스에 남아
있다"를 잡아냈다. codex가 놓친 소스 다섯 파일을 그 덕에 찾았다. **스펙과 구현이 어긋나면
빌드가 깨지는 구조가 실제로 작동한다.**

**codex가 놓친 것을 감독자가 보완한 10건.** 공통 패턴은 구현은 맞게 고쳤으나 그 변경이
깨뜨린 테스트를 끝까지 따라가지 못한 것이고, 직접 원인은 빌드를 돌리지 못한 것이다.

| # | 언어 | 무엇 |
|---|---|---|
| 1 | Java | catch만 지우고 `try`를 남겨 컴파일 불가 |
| 2 | Java | `CompletableFuture` 리스트에 `ZLinkBackendReceived::close` 참조 |
| 3 | Java | `tryEnqueue`가 false를 반환한다는 옛 단언 |
| 4 | Java | mailbox drain이 byte 예산으로 끊긴다는 옛 단언 |
| 5 | Node | worker thread로 보내는 함수가 바깥 변수를 캡처 |
| 6 | .NET | e2e 두 파일의 `CapacityExceeded` 잔존 |
| 7 | .NET | dispatcher 생성자를 `private`으로 막아 테스트 격리 불가 |
| 8 | .NET | 삭제된 lane capacity API를 쓰는 테스트 셋 |
| 9 | .NET | pending count가 0이 되는 시점을 잘못 봄 |
| 10 | .NET | `Backpressured`를 기대하는 옛 단언 셋 |

**환경에서 막혔던 것과 해결**

| 언어 | 막힌 이유 | 해결 |
|---|---|---|
| Node | 바인딩 패키지가 `0.17.6`이라 기존 TypeScript 오류 | 로컬 `0.18.0` 타르볼로 교체 |
| .NET | `ZLINK_LOCAL_PACKAGE_ROOT` 미설정으로 restore 실패 | `.artifacts/wsl` 지정 |
| Java | sandbox에서 Gradle 기동 실패 | JDK 25 + `--offline`로 정상 동작 |
| C++ | vcpkg 없음 | `~/.cache/zlink/vcpkg` bootstrap, worktree 공유 |

### 5.4 감독 기준

job이 끝날 때마다 다음을 확인한다.

1. **스펙 준수** — 인용한 `file:line`을 직접 열어 대조
2. **새 gap** — 스펙에 없는 동작을 지어냈는지, 다른 조항과 모순이 생겼는지
3. **규칙 수** — 줄었는지, 절대 참이 될 수 없는 분기를 남겼는지
4. **범위** — 남기기로 한 것(§3)을 건드렸는지, 스펙 문서를 고쳤는지
5. **교차언어** — 네 언어가 같은 결론인지, 한 언어만 다르면 이유가 구조적인지

동시 실행 수는 작업 순서와 남은 자원으로 정한다(`AGENTS.md` §2.1). 네 job은 서로 다른 언어·
디렉터리라 겹치지 않으므로 함께 돌릴 수 있고, load average가 코어 수의 절반을 넘으면 멈춘다.

## 6. gRPC 벤치와 0.90 — 이 계획의 종착점

이 작업은 백프레셔 정리에서 끝나지 않는다. **`#5` `#6` `#7`을 닫는 것까지가 범위다.**

### 6.1 닫아야 할 이슈

| Issue | 내용 | 막고 있는 것 |
|---|---|---|
| `#280` | C++ 포화 전환 | C++ request 벤치가 아예 안 돈다 |
| `#5` | .NET RouteMesh가 raw 대비 0.12~0.21 | 0.90 미달 |
| `#6` | Java RouteMesh가 raw 대비 0.02~0.07 | 0.90 미달 |
| `#7` | C++ RouteMesh가 raw 대비 0.006~0.065 | 0.90 미달 |
| `#277` | Node REQREP perf client가 drain timeout으로 측정 불가 | `#101`을 막고 있다 |
| `#101` | submit 결과 객체 perf 재측정과 릴리스 노트 | `#277` 해소 뒤 |

### 6.2 목표

`framework/bench/grpc/README.ko.md`가 규범이다. 언어마다 세 행을 낸다.

| 행 | 뜻 |
|---|---|
| `grpc-<lang>` | 그 언어의 gRPC unary RPC |
| `zlink-<lang>` | framework를 거치지 않는 raw binding의 ROUTER↔ROUTER |
| `zlink-framework-<lang>` | framework RouteMesh의 RID 직접 node request·send |

목표는 **framework ≥ 0.90 × 같은 언어 raw**다.

### 6.3 지금까지 알아낸 것 (2026-09-11, snapshot `00d5b25c`)

짝지어 5쌍, `framework / raw` 중앙값이다.

| 언어 | request | send |
|---|---|---|
| .NET | **1.1775 PASS** | 0.0677 / 0.1185 |
| Java | **1.3033 / 1.3142 PASS** | 0.0997 |
| C++ | 측정 불가 (`#280`) | 0.0608 / 0.0983 |

**request는 이미 목표를 넘겼다. 남은 것은 send다.**

send가 안 움직이는 이유는 깊이가 아니라 **건당 비용**이다. raw는 concurrency 1~2만 쓰고도
10배 빠르고, framework는 8을 다 쓰고도 느리다. 파이프가 꽉 찬 상태에서도 느리므로 건당
비용이 지배한다.

건당 비용 실측(`µs/message`, CPU core-seconds ÷ 메시지 수 중앙값, 1 KiB):

| 언어 | source raw → framework | target raw → framework |
|---|---:|---:|
| .NET | 4.44 → 108.50 (**24.4×**) | 1.98 → 75.72 (**38.2×**) |
| Java | 1.48 → 11.56 (**7.8×**) | 1.88 → 26.56 (**14.1×**) |
| C++ | 1.04 → 19.43 (**18.7×**) | 1.37 → 32.01 (**23.4×**) |

본체는 **typed encode, service-wire 포장, owner turn, 공개 completion 처리**다.

이미 들어간 것은 다시 열지 않는다 — `#241`(.NET `#48`), `#247`(Java `#47`), `#246`(Node `#50`),
`#245`(Node `#45`), `#256`(Java binding `#14`), `#265`(send 4지점).

### 6.4 순서

1. **`#280` 해소 확인** — C++ request 벤치 5쌍이 오류 없이 완주한다
2. **전체 비교표** — 언어 × {grpc, raw, framework} × {request, send} × {1 KiB, 4 KiB}.
   이번 백프레셔 변경 뒤 기준으로 **새로 잡는다**
3. **send 건당 비용 감축** — 언어별로 원인을 좁혀 `#5` `#6` `#7`을 닫는다. 한 job에 원인
   하나, 수정마다 재측정
4. **`#277` 해소 → `#101` 재측정** — Node REQREP가 풀려야 7언어 전수 재측정이 가능하다

### 6.5 준비 상태 (2026-09-12 확인)

| 항목 | 상태 |
|---|---|
| perf 큐 runner | 기동 중 |
| 벤치 빌드 | `framework/bench/grpc/build_all.sh` |
| 집계·비교 | `tools/bench_aggregate.py`, `tools/compare-results.py` |
| 규격 | `framework/bench/grpc/README.ko.md` — 기본 payload `1024,4096`, send concurrency `8`, loopback, Release build |

### 6.6 측정 규칙

- `scripts/perf/perf-ticket.sh submit`으로만 낸다. 직접 벤치를 돌리지 않는다
- 세션마다 `scripts/perf/perf-queue-runner.sh`를 하나 띄운다
- 짝지어 번갈아(raw 먼저, 그다음 framework), **최소 5쌍, 5개 값 전부 보고**
- `peak_in_flight`·`mean_in_flight`를 처리량·지연과 **함께** 읽는다. 처리량이 같은데 지연이
  달라 보이면 깊이를 재고 있는 것이다(Little's Law)
- **1-run 비교를 근거로 쓰지 않는다.** 과거에 세 번 결론을 뒤집었다
- 측정 중에는 같은 머신에서 빌드·테스트를 돌리지 않는다
- 벤치가 잘못 재고 있으면 gate가 아니라 벤치를 고친다. 상한을 넓히지 않는다

**주의** — 이번 변경으로 포화 시 거절 대신 대기하므로 수치가 달라진다. 인계 문서
`handoff-2026-09-12-framework-perf-and-hwm.ko.md` §1.2의 이전 수치와 직접 비교하지 않는다.

### 6.7 완료 조건

- `#280` `#281` `#282` `#283` 머지
- gRPC 포함 전체 비교표 게시
- `#5` `#6` `#7`의 send 비율이 0.90 이상
- `#277` 해소, `#101` 재측정 완료
