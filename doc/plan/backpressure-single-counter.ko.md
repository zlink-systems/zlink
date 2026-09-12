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
| `#277` bindings Node | 완료 | **실측으로 해소 확인** | [#289](https://github.com/zlink-systems/zlink/pull/289) |
| `#282` .NET | 완료 | **2,526개 전부 통과** | [#292](https://github.com/zlink-systems/zlink/pull/292) |
| `#280` C++ | codex 진행 중 | — | — |

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
| 11 | .NET | `ErrorKind` 개수를 13으로 고정한 wire 표와 계약 snapshot 잔존 |
| 12 | .NET | mailbox budget이 소스 다섯 파일에 남아 `ContractSurfaceCoverage` 실패 |
| 13 | .NET | 계약이 사라진 테스트 하나(`..._full_mailbox_returns_backpressured_terminal`) |

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
4. **`#101` 재측정** — `#277`이 풀렸으므로 7언어 전수 재측정을 할 수 있다

**`#277` 해소 확인 (2026-09-12).** perf 큐 티켓으로 `run_benchmarks_multi.sh`를 돌렸고, 이슈가
지목한 다섯 조합이 전부 측정된다. REQREP 결과 246행, `request completion drain timed out` 0건.

| 패턴 | 크기 | 결과 |
|---|---|---|
| `MULTI_DEALER_ROUTER_REQREP` tcp | 1024B | 120,473 msg/s, 0.726 ms |
| | 4096B | 93,076 msg/s, 0.678 ms |
| `MULTI_ROUTER_ROUTER_REQREP` tcp | 64B·1024B·4096B | 측정됨 |

같은 run에서 새로 드러난 SENDSEND 오류(`echoes=515002`인데 `admissions=79`)는 `#291`로 분리했다.

### 6.5 비교표를 채우는 자리

`framework/bench/grpc/doc/comparison.ko.md`가 이미 형식을 정해 두었다. 채울 것은 §4 기준선
상태 표와 §5 결과 표다.

| 행 이름 | 측정 경로 |
|---|---|
| `grpc-<lang>` | server 하나에 연결한 표준 gRPC unary `Echo`·`Command` |
| `zlink-<lang>` | framework를 거치지 않는 raw binding의 ROUTER↔ROUTER RID 직접 경로 |
| `zlink-framework-<lang>` | RouteMesh `requestToNode`·`sendToNode`로 server RID를 직접 지정 |

request 계열 단위는 KOPS, `send-saturation`은 KMSG/s다. C++의 `libgrpc++`는 시스템 설치본
1.51.1을 쓰며 오래된 버전이므로 결과에 반드시 적는다.

### 6.6 준비 상태 (2026-09-12 확인)

| 항목 | 상태 |
|---|---|
| perf 큐 runner | 기동 중 |
| 벤치 빌드 | `framework/bench/grpc/build_all.sh` |
| 집계·비교 | `tools/bench_aggregate.py`, `tools/compare-results.py` |
| 규격 | `framework/bench/grpc/README.ko.md` — 기본 payload `1024,4096`, send concurrency `8`, loopback, Release build |

### 6.7 측정 규칙

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

### 6.8 완료 조건

- `#280` `#281` `#282` `#283` 머지
- gRPC 포함 전체 비교표 게시
- `#5` `#6` `#7`의 send 비율이 0.90 이상
- `#277` 해소(**완료**), `#101` 재측정 완료
- `#291`(SENDSEND echo drain) 판정

### 5.x C++ (#280) 검증과 감독자 보완 4건 — 2026-09-12

codex job이 83 파일을 고쳐 빌드 93/93은 통과했지만 sandbox가 socket 생성·bind·DNS를
막아 테스트 21개를 돌리지 못했다. 감독 환경에서 다시 돌려 **환경 실패 21건 → 실제 결함
3건**으로 좁혔고, 넷을 고쳤다.

1. **wire decode 교차언어 불일치.** Java `ZLinkChannelEnvelope`, .NET `ZLinkErrorWireNames`,
   Node `channel-envelope.ts`는 `capacity_exceeded`를 인코딩·디코딩 표에서 모두 지웠는데
   C++ `envelope_codec.cpp:69`와 `request_failure_mapper.cpp:101`만 남겨 `unavailable`로
   매핑했다. 제거해 맞췄다 — 해독할 수 없는 error reply는 protocol error다.
2. **mailbox claim 예산을 queue 상한으로 오인.** `try_claim(…, 2, 1)`이 record 2개를
   준다고 고쳐놨다. claim의 byte 예산은 **receive turn 배치 예산**이라 그대로 남는다
   (`claim_owner_locked`는 첫 record를 무조건 담고 그 다음부터 예산을 본다). 되돌렸다.
3. **사라진 드랍 계약을 그대로 단언.** `owner_rejection_observability`가 "mailbox가 가득
   차면 두 번째 one-way를 버린다 + 드랍 계기 1 + `Target owner FIFO capacity exceeded`"를
   단언하고 있었다. "그대로 받는다 + 드랍 계기 0 + dropped 이벤트 없음 + 두 번째 record는
   여전히 claim된다"로 다시 썼다. 계기 `zlink.mesh_node.messages.dropped`는 스펙이
   요구하므로 남긴다 — backpressure가 더는 그 원인이 아닐 뿐이다.
4. **새 4,200-request 회귀가 동작하지 않음.** 완료 payload는 encode된
   `application_payload_t`인데 raw `bytes("reply")`와 비교했고, 앞 구간의 512 KiB
   saturating one-way가 이제 버려지지 않고 보존되는데 그걸 비우지 않아 reply token
   단언에서 멈췄다. 둘 다 고쳤다.

`test_cpp_framework_tooling_contract` 실패는 제품과 무관했다 — codex가 `/tmp/zlink-280-vcpkg`
toolchain으로 configure해 둬서 isolated configure가 Core package를 못 찾았다. 올바른
`VCPKG_ROOT`·`ZLINK_LOCAL_PACKAGE_ROOT`로 직접 돌리면 8초에 통과한다.

최종: `cmake --build build/ci -j10` 93/93, `ctest -L 'framework-unit|framework-contract|
http-client-unit|http-client-contract'` **67/67**. PR **#294**.

### 5.y 스펙에 남아 있던 상한 표현 (ab657858f0)

C++ 구현을 검토하다 per-language 문서에 상한이 남은 것을 찾았다. `set_max_pending`
(runtime 전체 pending queue 상한), `worker_options_t`·`Worker`의 queue 상한, C++ §6.1의
"기본 정책은 무한 queue가 아니다", Spot publish §4.5의 "queue 용량 부족". 모두 걷어냈고
`verify-framework-doc-contracts.sh` CLEAN이다.

Node PR에는 NestJS 동반 package의 `ZLinkWorkerOptions.maxQueueLength`가 남아 있었다.
framework 본체에서는 지웠는데 동반 package가 같은 필드를 **필수로** 요구한 채였다.
지우고 `npm run typecheck` 통과 — #288에 포함.

### 5.z codex 호출 형태 교정

`codex --help`를 제대로 읽지 않아 생긴 손실이 있었다. `-s danger-full-access`와
`--add-dir`를 주지 않아 #280 job이 테스트를 못 돌렸고, `codex exec resume <session_id>`를
몰라 라운드 1이 틀렸을 때 브리프를 다시 쓰고 4개 job을 처음부터 재실행했다.
리뷰도 `codex exec review --base main`이라는 전용 서브커맨드가 있었다.
이후 job은 모두 교정된 형태로 띄운다.

## 7. send 건당 비용 — 조사 결과 (2026-09-12)

언어마다 따로 조사를 돌렸는데 **세 언어가 같은 곳을 가리켰다.** 언어별 버그 세 개가 아니라
같은 설계 비용이 세 번 나타난 것이다.

| 원인 | .NET | Java | C++ |
|---|---|---|---|
| source state lane·socket gate 공유 — 직렬화 | **1위** | 3위 | 3위 |
| service-wire 재포장 — body 전체 복사 (source 2회, target 3회) | | 2위 | **1위** |
| 즉시 수락된 send에도 completion graph를 만든다 | | **1위** | 2위 |

원본은 `doc/plan/send-cost-worklog/findings-{dotnet,java,cpp}.md`.

**.NET 조사가 성질이 다른 것을 짚었다.** 나머지 둘이 건당 비용을 본 반면 .NET은
**직렬화**를 지목했다: framework는 8개 logical stream이 한 RouteMesh node의 state lane과
`_socketGate` 하나를 공유하는데, raw는 socket을 8개 만들어 각자 자기 semaphore만 쓴다
(`ZLinkManagedMeshNode.cs:9017,11372`, `ZLinkStateLane.cs:101` 대 `Client/Program.cs:570,674`).
건당 비용을 줄여도 이 목이 남으면 concurrency를 못 쓴다. 그래서 이것이 1순위다.

감독자가 main에서 확인했다. `ZLinkManagedMeshNode.cs:9116` `RequireDirectPeer`는
`_lane.IsOnLane ? resolve() : RunState(resolve)`라 lane 밖 caller가 매 send마다 state
lane을 거치고, `:9159` `SendDirectWireAsync`는 모든 sender가 `lock (_socketGate)` 안에서
socket submit builder를 만든다. 8개 stream이 두 곳을 공유한다.

같은 파일 `:9125` `CreateApplicationWire`가
`ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(parts)`를 호출하므로
**재포장 복사는 .NET에도 있다.** 세 언어 전부다.

### 7.1 감독자가 코드로 확인한 것

**Java — 1 KiB body가 source에서만 4번 복사된다.**
`ZLinkProtobufMessageSerializer`가 `toByteArray()`(복사 1),
`ZLinkEncodedPayload.from()`이 `Arrays.copyOf`(2), `bytes()`가 또 `Arrays.copyOf`(3),
`Message.from(byte[])`가 native로 복사(4). target은 `ZLinkMessagePayloads.encoded()`가
`message.toByteArray()`부터 같은 계단을 되풀이한다.
(`ZLinkEncodedPayload.java:15,19`, `ZLinkMessagePayloads.java:13,18`)

**C++ — service-wire 포장이 body를 두 번 통째로 쓴다.**
`application_payload_t::from_parts`가 part마다 `part.copy()`
(`service_wire_codec.cpp:2973`), 이어 `encode_application_payload`가 그 전부를 새
vector로 다시 쓴다(`:3062`). target은 outer frame 복사 + inner multipart `assign` +
part별 `message_t` 재생성으로 세 번이다.

raw는 같은 protobuf DTO와 같은 두 part를 쓰는데도 이 계단이 없다. 그래서 **건당 비용
차이는 wire 모양이 아니라 그 모양을 만드는 과정에서 생긴다.**

### 7.2 건드리지 않을 것 — 두 조사가 같은 선을 그었다

- **send의 완료 의미를 바꾸지 않는다.** send는 source-local queue 수락으로 완료하고
  remote handler를 기다리지 않는다(`00-foundation/04-interaction-model` §, C++
  `03-channel-messaging` §). completion graph를 줄이는 것은 같은 결과를 더 싸게
  표현하는 일이지 target ACK를 더하는 일이 아니다.
- **wire 형식을 바꾸지 않는다.** `nodeSend` command와 Framework multipart profile의
  part 순서·길이 형식은 그대로다(`02-channel-transport/06-wire-protocol` §2).
  최적화 대상은 형식이 아니라 **형식 사이의 불필요한 materialization**이다.
- **target permit과 handler turn을 우회하지 않는다.** permit을 먼저 얻고 handler
  turn으로 옮기는 순서는 이번 백프레셔 설계 그 자체다
  (`01-execution/04-application-job-queue-and-backpressure` §3).
- **binding의 DONT_WAIT 재시도를 Framework가 다시 구현하지 않는다.**

### 7.3 다음

건당 비용 감축은 **기준선을 새로 잡은 뒤**에 손댄다(§6.4 순서 2 → 3). 지금 고치면
무엇이 좋아졌는지 말할 수 없다. 순서는 위 표의 1·2위를 언어마다 하나씩, 수정마다 재측정.

### 7.4 `#280` 해소 실측 (2026-09-12 22:09)

C++ request 벤치가 오류 없이 완주한다. RESULT 27행, 오염 0건. 측정 자체가 불가능하던
상태는 끝났다.

5초 1-run 값(`request-backpressure`, 1 KiB)이라 **판정에 쓰지 않는다.** 그래도
기록해 둔다 — 5-run 기준선이 이 그림을 확인하는지 봐야 하기 때문이다.

| 행 | throughput | latency |
|---|---:|---:|
| `grpc-cpp` | 39,557 | 407.4 |
| `zlink-cpp` (raw) | 375,717 | 0.389 |
| `zlink-framework-cpp` | 19,573 | 250.4 |

framework/raw = 0.052. Java·.NET의 request는 1.18~1.31로 목표를 넘겼는데 C++만 다르다.
latency가 raw의 640배라 건당 비용이 아니라 **직렬화**로 보이고, 이는 §7의 source
state lane 지적과 같은 방향이다. 5-run 기준선으로 확인한다.

## 8. 기준선 1차 — C++ (2026-09-12 22:13~22:30)

5 run, rc=0 전부. 집계는 `tools/bench_aggregate.py`가 소유한다(중앙값·G5·§7.2 비율,
그리고 **그 비율을 공개해도 되는지의 판정**까지). 감독자가 손으로 계산하지 않는다.

| 패턴 | 1 KiB | 4 KiB | 판정 |
|---|---:|---:|---|
| `request-serial` | 0.439 | 0.440 | published — **fail** |
| `send-saturation` | 0.064 | 0.105 | published — **fail** |
| `request-backpressure` | (0.087) | (0.072) | unsupported — G5 90.2%·99.7% |

`zlink-framework-cpp / zlink-cpp`다. 목표는 0.80(§7.2 문턱) 이상, 계획 목표는 0.90.

### 8.1 집계기 결함 — send 판정이 통째로 막혀 있었다

`_structured_cells`가 `<run>/*.json`만 훑는데 모든 언어 runner는 셀 문서를
`<run>/<cell>/results.json`에 쓴다. 그래서 구조화된 셀 데이터를 한 번도 읽지 못하고
`report.txt` 파싱으로 떨어졌고, 거기엔 `server_received_at_close`가 없다.
G3가 send 셀을 전부 "client 집계"로 판정해 **모든 언어의 모든 send 비율이 공개
불가**였다. 산출물에는 값이 다 있었다.

한 줄 고침(디렉터리 한 단계 아래도 훑는다), `tools/tests` 71건 통과. 고친 뒤 C++
send 비율이 published가 됐다. **이걸 안 고쳤으면 send를 최적화하고도 결과를 공개할
수 없었다.**

### 8.2 `request-backpressure`가 재현되지 않는다

C++ framework가 5 run 사이에 90.2%·99.7% 흔들린다(한도 10%). 이건 느리다는 문제가
아니라 **같은 조건에서 같은 값이 안 나온다**는 문제다. §7의 source state lane
직렬화 지적과 맞는 모양이고, 개선 항목 목록에 넣는다.

### 8.3 C 기준 벤치 부패 → `#295`

`framework/bench/grpc/c`가 Core에서 사라진 C API(`zlink_part_flag_t`,
`zlink_router_recv_part`, `ZLINK_PART_FINAL/MORE`)를 쓴다. `zlink-c` 행을 못 내므로
§7.2 formula 1이 모든 언어에서 `unsupported`로 남는다. 0.90 목표는 같은 언어 안의
비율이라 영향이 없어서 send 개선 작업을 막지는 않는다.

`build_all.sh`가 `set -euo pipefail`이라 C에서 멈추면 나머지 언어 빌드까지 막히는
것도 같은 이슈에 적었다.

## 9. 기준선 — 세 언어 (2026-09-12 22:13~23:40)

`zlink-framework-<lang> / zlink-<lang>`, 5 run, 집계는 `bench_aggregate.py`가 소유한다.
`request-backpressure`가 README §7.2의 **판정 기준 패턴**이다.

| 패턴 | Java | .NET | C++ |
|---|---|---|---|
| **`request-backpressure`** | **1.291 / 1.255 pass** | 1.309 pass / (0.948) G5 31.4% | (0.087) G5 90.2% / (0.072) G5 99.7% |
| `request-serial` | 0.547 / 0.509 fail | (0.384) G5 16.0% / 0.409 fail | 0.439 / 0.440 fail |
| `send-saturation` | 0.125 fail / (0.203) G6 | 0.102 / 0.190 fail | 0.064 / 0.105 fail |

### 9.1 request는 이미 목표를 넘겼다 — C++만 빼고

Java가 두 크기 모두 통과(1.291·1.255)다. framework가 raw보다 빠르다. .NET도 1 KiB에서
1.309로 통과한다. **C++만 0.087·0.072로 두 자릿수 배 뒤처진다.**

그런데 **C++만 벤치 대기 정책이 정본과 어긋나 있었다**(`#296`). 고정 1ms를 쓰는 경로는
`run_unbounded` 하나뿐이고 그게 `_window <= 0`인 `request-backpressure`다. G5가 깨진 패턴도
정확히 그 하나다 — `request-serial`·`send-saturation`은 이미 블록하고 있었고 둘 다 G5를
통과했다. **그러므로 C++의 0.087은 framework 성능이 아니라 하네스가 만든 숫자일 수 있다.**
`#296`을 고치고 재측정하기 전에는 `#7`을 판정하지 않는다.

### 9.2 send는 세 언어 전부 실패

0.064~0.190이다. 계획의 전제가 맞고, §7의 직렬화·재포장 복사 지적이 여기에 걸린다.

### 9.3 G6 — raw가 CPU에 막히면 분모로 못 쓴다

Java send 4 KiB는 `zlink-java`가 선언한 JVM thread core의 **0.97까지 포화**라 분모 자격이
없다(spec 5.1, G6). raw가 CPU에 막힌 상태의 값은 계층 비용이 아니라 그 조건을 잰 값이다.
send를 개선해도 이 크기에서는 판정이 나오지 않으므로, 분모 쪽 포화를 먼저 풀어야 한다.

### 9.4 runner마다 인자 규약과 출력 위치가 다르다

- C++: 위치 인자로 run label, `OUTPUT_DIR`
- .NET: `OUTPUT` 환경변수, 위치 인자를 거부한다(`unsupported runner argument`)
- Java·Node: `OUTROOT`+`RUNS`, 그리고 **상대 경로를 자기 디렉터리 기준으로 푼다** —
  `OUTROOT=framework/bench/grpc/log/java/base`를 주면 결과가
  `framework/bench/grpc/java/framework/bench/grpc/log/java/base`에 생긴다

첫 .NET 5건은 C++ 규약을 그대로 써서 전부 즉시 실패했다. 측정 티켓을 내기 전에 그 언어
문서의 입력 표를 확인한다.

### 9.5 Node는 재실행이 필요하다

run 1은 18셀 전부 완주했고 run 2의 `zlink-node-request-serial-1024`에서 멈췄다 —
source가 뜨고 trigger까지 받은 뒤 출력 없이 hang(`source phase did not complete`).
run 1의 같은 셀은 정상이라 간헐적이다. 한 번으로 단정하지 않고 재실행해 재현 여부를 본다.

## 10. 순서가 바뀐다 — 측정 장치를 먼저 고친다

기준선을 내면서 하네스 결함 세 건이 나왔다. 셋 다 성격이 같다. **framework 성능이 아니라
측정 장치가 만든 숫자다.** 고치기 전에 send를 최적화하면 무엇이 좋아졌는지 말할 수 없다.

| 이슈 | 내용 | 막는 것 |
|---|---|---|
| `#295` | C 기준 벤치가 `#63`의 whole-message 이관에서 빠졌다 | 분모(raw)가 건강한지 판정 불가 |
| `#296` | C++ 벤치가 빈 라운드마다 고정 1ms 대기 | C++ `request-backpressure` G5 90~100% |
| `#300` | .NET raw가 `request-backpressure`에서 깊이 132로 자기 제한 | .NET "1.309 통과"가 무효 |

### 10.1 `#300` — 분모가 자기 발에 걸렸다

`request-backpressure`는 응용 상한 없이 파이프가 찰 때까지 보내는 패턴이다. 그런데:

| 행 | peak in-flight | throughput | latency | client CPU |
|---|---:|---:|---:|---:|
| `zlink-dotnet` (raw) | **132** | 36.3 KOPS | 0.206 ms | 6.1% |
| `zlink-framework-dotnet` | 6,717 | 47.5 KOPS | 29.9 ms | 16.4% |
| `zlink-cpp` (raw, 참고) | — | 382.2 KOPS | 0.380 ms | 9.1% |

raw가 지연은 더 낮은데 처리량이 C++ raw의 1/10이고 CPU는 6%다. 막힌 것이 아니라 깊이를
못 쌓는다. 그러면 `zlink-framework-dotnet / zlink-dotnet = 1.309 pass`는 framework가 빠른
것이 아니라 **분모가 느려서 나온 값**이다.

### 10.2 `#295`의 우선순위를 올린다 — 앞선 판단을 정정한다

§8.3에서 "`#295`는 0.90 목표에 영향이 없으니 send 개선을 막지 않는다"고 적었다.
**부정확했다.** 두 번째 식만 보면 그렇지만, 분모가 건강한지 가려내는 것이 첫 번째 식
`zlink-<lang> / zlink-c`다. `#300`이 보여주듯 분모가 병들 수 있고, 그러면 통과 판정 자체가
성립하지 않는다. `#295`는 막지 않는 것이 아니라 **판정의 전제**다.

### 10.3 새 순서

1. `#295` C 기준 벤치 이관 — 빌드·검증·측정
2. `#296` C++ 대기 정책 — 빌드·검증 → C++ 재측정
3. `#300` .NET raw 깊이 — 조사·수정 → .NET 재측정
4. Node 기준선 확보(`#9.5` hang 재현 여부)
5. 그 뒤에 `#5` `#6` `#7` — send 건당 비용·직렬화

1~3이 끝나야 비교표의 숫자가 판정에 쓸 수 있는 값이 된다.

## 11. 하네스 4건 검증 결과 (2026-09-13)

### 11.1 `#295` 해결 — `zlink-c`가 돌아왔고 그림이 뒤집혔다

C 벤치를 whole-message로 옮겨 빌드·스모크 통과(오류 0건). PR **#302**.

`zlink-c-request-backpressure` 1 KiB = **603.4 KOPS**. 여기에 §7.2 첫 번째 식
(`zlink-<lang> / zlink-c`, 통과선 0.80)을 대면:

| binding | throughput | `/ zlink-c` |
|---|---:|---:|
| `zlink-c` | 603.4 | — |
| `zlink-cpp` | 382.2 | 0.63 |
| `zlink-dotnet` | 36.3 | **0.06** |
| `zlink-java` | ~45 | **0.07** |
| `zlink-node` | 2.8~13.1 | **0.005** |

**binding 계층이 전부 통과선 아래다.** 계획은 "framework가 raw 대비 0.90"을 쫓고 있었는데
**raw 자체가 C API 대비 0.5~7%**였다. 그 위에서 잰 framework 비율은 의미가 없었고,
Java의 "1.291 통과"는 0.07짜리 분모 위에 세운 값이었다.

`zlink-c`가 없었기 때문에 이 사실 자체를 볼 수 없었다. §10.2의 정정이 옳았다.

### 11.2 `#296` 부분 해결 — 대기 정책은 맞았고, 남은 변동은 framework다

정본 대기 정책으로 교정 뒤 3 run:

| G5 편차 | 수정 전 | 수정 후 |
|---|---:|---:|
| 1 KiB | 90.2% | 68.0% |
| 4 KiB | 99.7% | 67.2% |

개선했지만 한도 10%에는 못 미친다. **고정 1ms는 원인의 일부였다.** 앞서 "증상과 원인이
정확히 일치한다"고 적은 것은 과했다 — 일치한 것은 어느 패턴에서 나는가였다.

run별로 보면 원인이 갈린다.

| run | `zlink-cpp` (raw) | `zlink-framework-cpp` | fw peak | fw CPU |
|---|---:|---:|---:|---:|
| 1 | 389.4 KOPS | 26.2 | 18,039 | 11.0% |
| 2 | 375.3 | 34.0 | 22,587 | 13.3% |
| 3 | 377.8 | **8.4** | 19,294 | **7.1%** |

**raw는 편차 3.7%로 완벽히 재현된다.** 같은 하네스·같은 대기 정책이다. 흔들리는 것은
framework 행뿐이고 깊이는 비슷한데 처리량이 4배 갈린다. run3은 깊이가 쌓였는데 CPU가
7.1%다 — 일감은 있는데 처리를 못 한다.

그러므로 남은 변동은 측정 장치가 아니라 **framework 쪽**이고, §7이 지목한 source state
lane 직렬화의 **직접 증거**다. 직렬화된 경로는 스케줄링에 따라 처리량이 크게 갈린다.
남은 G5 실패는 `#7`의 증상으로 옮긴다 — "재현되지 않는다"는 "느리다"와 다른 정보다.

### 11.3 `#300` 수정 입증 — 그리고 새 결함을 드러냈다

permit 1개 게이트를 없애고 정본 구조(제출 즉시 reply 관찰, `Backpressured`일 때만
admission을 별도 task로)로 바꿨다.

| | 전 | 후 |
|---|---:|---:|
| `peak_in_flight` | 132 | **4,226** |
| 5초 창 완료 | 174,792 | **885,367** |

그런데 이 수정이 회계 구멍을 드러냈다 — target이 받고 응답까지 한 request 11건을 source가
`completed`로도 `abandoned`로도 세지 않는다(`errors`도 0). 깊이 132에서는 창이 좁아 보이지
않던 것이다. `#303`으로 분리했고, **읽기만으로 행방이 확정되지 않아 추측을 적지 않았다.**
이 계수 게이트가 통과해야 `#300`을 land할 수 있다.

warmup→active 경계는 README §3의 settle 계약(server 받은 수가 멈출 때까지 대기)을 그
경계에도 적용해 19 → 11로 줄였다. 새 허용치를 만든 것이 아니라 이미 있는 규칙을 한 군데 더
적용한 것이다.

### 11.4 남은 것

| 이슈 | 상태 |
|---|---|
| `#295` | 해결 — PR #302 |
| `#296` | 대기 정책 교정 완료; 남은 G5 실패는 `#7`로 이관 |
| `#300` | 깊이 수정 입증; `#303`에 막혀 land 대기 |
| `#303` | .NET request 계수 구멍 — 조사 필요 |
| `#301` | Node raw가 event loop 1.5%만 사용 — 미착수 |

그 뒤에 `#5` `#6` `#7`이다. 지금 시점의 결론은 **binding 계층(`#91`~`#93` 계열)이
framework보다 먼저라는 것**이다 — 분모가 0.005~0.07인 상태에서 분자를 고쳐도 판정이 없다.

## 12. 하네스 복구 완료 — 그리고 계획의 전제가 틀렸다는 것 (2026-09-13)

측정 장치 결함 5건을 모두 고쳤다. 전부 main에 들어갔다.

| 이슈 | 무엇 | PR |
|---|---|---|
| `#295` | C 기준 벤치가 `#63` whole-message 이관에서 빠졌다 | #302 |
| `#296` | C++ 벤치가 빈 라운드마다 고정 1ms 폴링 | #305 |
| `#300` | .NET·Java가 제출을 permit 1개 게이트로 직렬화 | #304 |
| `#303` | .NET이 완료한 request 일부를 계수에서 잃음 | #304 |
| `#301` | Node raw가 event loop를 1.5%만 사용 | #306 |

### 12.1 넷이 같은 병이었다

| 언어 | 증상 | 기전 |
|---|---|---|
| .NET | 깊이 132 | permit 1개 게이트 안에서 `await Admitted` |
| Java | 깊이 35~130 | 같은 계열 |
| Node | ELU 1.5% | backpressure에서 제출 루프 정지 |

셋 다 **"backpressure를 만나면 제출을 멈추고 기다린다"**다. 정본
(`bindings/c/perf`, `bindings/node/perf`)은 **"그 socket 하나만 건너뛰고 나머지는 계속
제출한다"**다.

**이번에 스펙으로 확정한 원칙을 벤치 클라이언트들이 정확히 어기고 있었다** — 가득 차면
기다리되 멈추지는 않는다. 우연이 아니라 같은 오해가 여러 곳에 퍼져 있었던 것으로 본다.

### 12.2 binding 계층 회복

| binding | `/ zlink-c` 전 | 후 | 배수 |
|---|---:|---:|---:|
| Java | 0.07 | **0.49** | 7.0× |
| .NET | 0.06 | **0.31** | 5.2× |
| Node | 0.005 | **0.147** | 29× |
| C++ | 0.63 | 0.63 | — |

`zlink-c` = 603.4 KOPS(1 KiB, `request-backpressure`). 통과선은 0.80이다.
**아직 넷 다 통과선 아래다.**

### 12.3 계획의 전제가 틀렸다

이 계획은 §6.2에서 **"framework ≥ 0.90 × 같은 언어 raw"**를 목표로 삼았다. 그런데
`zlink-c`를 복구해 보니 **raw 자체가 C API 대비 0.005~0.07**이었다. 분모가 그 상태면
분자를 고쳐도 판정이 없다. 실제로 Java의 "1.291 통과"는 0.07짜리 분모 위에 세운 값이었다.

`#295`를 처음에 "0.90 작업을 막지 않는다"고 판단한 것이 정확히 뒤집혔다. `zlink-c`가
없었기 때문에 **이 사실 자체를 볼 수 없었다.**

**그러므로 순서는 binding 계층이 먼저다.** §7.2의 두 식은 순서가 있다 —
`zlink-<lang> / zlink-c >= 0.80`(binding)이 서고 나야
`zlink-framework-<lang> / zlink-<lang> >= 0.80`(framework)이 뜻을 갖는다.

### 12.4 다음

1. 전 언어 재측정(main 기준, 5 run) — 하네스 복구 뒤의 진짜 기준선
2. `comparison.ko.md` §5 확정
3. binding 계층이 아직 0.15~0.49다. `#91`~`#93`(submit 종결자 결과 객체)은 **이미 닫혔고**
   이번 하네스 복구도 끝났으므로, 남은 격차는 **새로 조사해야 한다** — 하네스가 아니라
   binding 자체의 건당 비용인지 확인한다
4. 그 뒤 `#5` `#6` `#7` — send 건당 비용·직렬화(§7 조사 결과가 이미 있다)

`#296`에서 남은 C++ framework의 G5 실패(68%)는 `#7`의 증상으로 옮겼다. raw가 같은
하네스에서 3.7%로 재현되므로 그것은 framework 쪽 직렬화의 직접 증거다.

## 13. 재측정 — 두 판정식이 처음으로 함께 나왔다 (2026-09-13)

### 13.1 집계기 결함이 하나 더 있었다

C runner는 `with_grpc_c_<stamp>.txt`를 쓰는데(`run_local.sh:9` `REPORT_FILE`) 집계기가
`report.txt`만 찾았다. **`#295`로 C 벤치를 되살린 뒤에도 도구가 `zlink-c` 행을 통째로
놓치고 있었다.** 그래서 formula 1이 계속 비어 있었다. reader를 고쳤다(파일 이름은 schema가
아니다 — 디렉터리 깊이 때와 같은 판단). `tools/tests` 71건 통과.

### 13.2 .NET — 첫 완전한 판정 (1 KiB)

```
zlink-dotnet / zlink-c                = 0.300   binding 계층      fail
zlink-framework-dotnet / zlink-dotnet = 0.266   framework 추가 비용  fail
                                        ────
                              C API 대비  0.08
```

4 KiB는 framework 식이 0.168 fail, binding 식은 분모 G5 44.8%로 unsupported다.

**두 계층이 각각 3~4배씩 잃는다.**

### 13.3 "1.309 통과"가 0.266 실패로 뒤집혔다

| .NET 1 KiB | 하네스 수정 전 | 후 |
|---|---:|---:|
| `zlink-dotnet` (raw) | 36.3 KOPS | **180.2** |
| `zlink-framework-dotnet` | 47.5 | 47.9 |
| framework/raw | **1.309 "통과"** | **0.266 fail** |

framework 값은 거의 그대로인데 분모가 5배 올라 비율이 뒤집혔다. §10.1에서 "분모가 자기
발에 걸린 값"이라고 판단한 것이 실측으로 확인됐다. **하네스를 고치지 않았다면 .NET을
통과로 기록하고 넘어갔을 것이다.**

### 13.4 `#309` — Core byte HWM 회계가 언더플로로 abort한다

`#300`으로 깊이 제한을 풀자 in-flight가 132 → **25,909**가 됐고, 그 부하에서 Core가 죽었다.

```
Assertion failed: current >= amount_
  (core/src/runtime/core/ctx_physical_queue_registry.cpp:64)
```

5 run 중 1건에서 target이 abort해 `Connection refused`, `received=0`이 됐다. 간헐적이다.

**이번에 스펙으로 확정한 두 카운터 중 하나가 높은 깊이에서 언더플로한다.** 깊이가 두
자릿수로 묶여 있던 동안에는 이 결함이 보이지 않았다.

### 13.5 측정 신뢰성 문제 총 8건

| 이슈 | 무엇 | 상태 |
|---|---|---|
| `#295` | C 기준 벤치가 `#63` 이관에서 빠짐 | 해결 |
| `#296` | C++ 고정 1ms 폴링 | 해결 |
| `#300` | .NET·Java 제출 직렬화 | 해결 |
| `#301` | Node raw event loop 1.5% | 해결 |
| `#303` | .NET 계수 누락 | 해결 |
| 집계기 §8.1 | 셀 JSON을 디렉터리 깊이 때문에 못 읽음 | 해결 |
| 집계기 §13.1 | C report 파일 이름을 몰라 `zlink-c` 누락 | 해결 |
| `#307` | 로컬 패키지가 소스보다 뒤처짐 | 열림 |
| `#308` | C와 언어 행이 다른 Core 바이너리 | 열림 |

계획이 쫓던 "framework 0.90 미달"이라는 숫자는 이 여러 겹 위에 있었다.

### 13.6 조사 결론 — 두 계층의 공통 비용

| 계층 | 매 메시지마다 하는 일 | 근거 |
|---|---|---|
| binding | 정상 admission에도 language request state 객체 + registry 항목 (C++ bundle/entry, Java `Pending`, .NET `RequestCompletionEntry`, Node `CompletionEntry`) | `findings-binding-gap.md` §3 |
| framework | 직렬화(state lane·socket gate) + service-wire 재포장 전체 복사 | `findings-{cpp,java,dotnet}.md` §2 |

둘 다 **정상 경로에서도 매 메시지마다 만드는 것**이 핵심이다. C 하네스는 slot pointer와
카운터만 쓴다.

## 14. Java·Node — 고치니 반대편 한계가 드러났다 (2026-09-13)

`#300`·`#301`로 제출 직렬화와 event loop 정지를 없앤 뒤 두 언어가 **판정 불가로
바뀌었다.** 이전에는 "일을 안 해서" 값을 못 냈고, 이제는 "단일 실행 단위를 다 써서"
못 낸다.

| 행 | 포화 지표 | 선언 상한 | 실측 | 결과 |
|---|---|---:|---:|---|
| .NET | `client_cores` (프로세스 CPU) | **20** (논리 코어) | 2.61 | 13% — 통과 |
| Java | `jvm_thread_cores` (submit 스레드 CPU) | **1** | 0.98 | 98% — G6 차단 |
| Node | `event_loop_utilization` | **1** | 1.00 | 100% — G6 차단 |

### 14.1 이전 "통과"가 전부 뒤집혔다

| framework/raw (1 KiB) | 하네스 수정 전 | 후 |
|---|---:|---:|
| .NET | **1.309 통과** | **0.266 실패** |
| Java | **1.291 통과** | (0.041) G6 |
| Node | (2.602) G5 324% | (0.090) G6+G5 |

**분모가 놀던 값이었다.** .NET은 raw가 36.3 → 180.2 KOPS로 5배 올랐는데 framework는
47.5 → 47.9로 그대로다. 그래서 비율이 뒤집혔다.

하네스를 고치지 않았다면 .NET과 Java를 **통과로 기록하고 넘어갔을 것이다.**

### 14.2 `#310` — 이건 버그가 아니라 규격 결정이다

`zlink-<lang> / zlink-c`는 binding 계층 비용을 재는 식인데, 지금은 **단일 실행 단위 대
C 하네스의 여러 client**를 재고 있다.

- **Node**는 event loop가 하나뿐이라 상한 1이 **구조적**이다. 하네스 설정으로 바꿀 수 없다.
- **Java**는 driver가 submit 병렬도 1을 선언한 설계다(`ClientResources.java:36-43`).
- **.NET만** 논리 코어 수 20을 선언해 통과한다.

세 언어가 서로 다른 잣대로 "포화"를 판정한다. 그 상태의 비율은 계층 비용이 아니라
**실행 모델 차이**를 잰다. 무엇을 기준으로 삼을지는 감독자가 정할 사항이 아니므로
`#310`에 선택지를 적어 두었다.

### 14.3 지금 판정이 나오는 언어

**.NET 하나뿐이다.**

```
zlink-dotnet / zlink-c                = 0.300  fail
zlink-framework-dotnet / zlink-dotnet = 0.266  fail
                                        ────
                              C API 대비  0.08
```

Java·Node는 G6, C++는 재측정 중이다.

## 15. send 비용 1차 시도 — 효과 없음 (2026-09-13)

### 15.1 `#7` C++ — source 쪽 복사 1회 제거는 움직이지 않는다

§7 조사가 C++의 1순위로 지목한 **service-wire 재포장 복사** 중 source 쪽 1회를 없앴다.
`from_parts`를 rvalue 오버로드로 만들어 `part.copy()`를 제거하고, `app.cpp` →
`mesh_node_runtime` → `public_host_runtime` → codec 으로 소유권을 이동 전달했다.
wire 형식과 outer frame materialization은 그대로 뒀다. `ctest -L 'framework-unit|
framework-contract'` **67/67 통과**.

| `zlink-framework-cpp / zlink-cpp` (send-saturation) | 기준선 | 수정 후 |
|---|---:|---:|
| 1 KiB | 0.064 | **0.065** |
| 4 KiB | 0.107 | **0.105** |

**측정 오차 범위다. 효과가 없다.**

### 15.2 이것이 측정 기준을 세운 값어치다

**효과 없음을 효과 없음이라고 말할 수 있다.** 기준선이 없었다면 "복사를 줄였으니
빨라졌겠지"로 넘어갔을 것이고, 다음 수정도 같은 식으로 쌓였을 것이다. 실제로 이 세션
전까지의 상태가 그랬다 — .NET과 Java가 "1.309 통과", "1.291 통과"로 기록돼 있었다.

### 15.3 다음은 어디를 볼 것인가

§7 조사가 C++에 매긴 순위는 ① 재포장 복사 ② 즉시 수락된 send의 completion graph
③ source state lane이었다. ①을 **부분적으로**(source 1회) 건드려 변화가 없으므로:

- 남은 복사는 **target 쪽 3회**다(outer frame 복사 + inner multipart `assign` + part별
  `message_t` 재생성). source 1회보다 크다.
- 또는 ②·③이 실제 비용이다. `#296`에서 확인한 **framework 행만 4배씩 흔들리는 현상**은
  ③(직렬화)의 직접 증거였다.

한 번에 하나씩 고치고 매번 측정한다는 §7.3의 방식은 유지한다. 이번 시도가 그 방식이
실제로 작동함을 보여줬다 — 틀린 가설을 싸게 버렸다.

### 15.4 `#5` .NET — 1단계 완료, 측정 대기

`_socketGate`를 socket 참조 확보용 짧은 경계로만 남기고 submit을 밖으로 뺐다.
`RequireDirectPeer`의 lane 왕복(2단계)은 손대지 않았다. 단위 테스트 **2,177/2,177 통과**.

처음 실행에서 1건 실패했으나 `#7` 측정과 겹친 부하 때문이었고 격리 재실행으로 통과했다
([[env-gates-load-sensitive-timing]] 규칙대로 허용치를 넓히지 않았다).

### 15.5 `#5` .NET도 효과 없음 — 이건 신호다

`_socketGate`를 socket 참조 확보용 짧은 경계로만 남기고 submit을 밖으로 뺐다.
단위 테스트 2,177/2,177 통과.

| `zlink-framework-dotnet / zlink-dotnet` (send-saturation) | 기준선 | 수정 후 |
|---|---:|---:|
| 1 KiB | 0.104 | **0.101** |
| 4 KiB | (0.193) | **0.190** |

**두 언어 모두 1차 시도가 측정 오차 범위다.** C++은 조사 1순위(재포장 복사)를, .NET은
조사 1순위(`_socketGate` 직렬화)를 각각 건드렸는데 둘 다 안 움직였다.

**우연으로 보기 어렵다.** 조사가 코드에서 읽어낸 "비싸 보이는 것"이 실제 지배적 비용이
아니라는 뜻이다.

크기를 보면 그렇다 — `send-saturation`에서 raw는 822 KMSG/s인데 framework는 52다.
**16배다.** 한두 군데의 할당이나 lock으로 설명되는 크기가 아니다.

### 15.6 다음은 코드 읽기가 아니라 계측이다

조사 세 건(§7)은 코드를 읽어 후보를 세웠고, 그 방식으로 뽑은 1순위 둘이 모두 빗나갔다.
**다음은 프로파일러나 계측으로 실제 비용 분포를 봐야 한다.**

읽기로 세운 가설을 계속 시도하면 이번처럼 하나씩 버리는 데 각각 한 번의 구현·측정
왕복이 든다. 두 번 해봤으니 방식을 바꿀 근거가 생겼다.

측정 기준이 섰기 때문에 이 결론에 도달할 수 있었다. 기준선이 없었다면 두 수정 모두
"복사를 줄였으니", "lock을 뺐으니" 개선으로 기록됐을 것이다.
