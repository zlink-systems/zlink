# 인계 — framework 성능(gRPC bench)과 HWM 흐름 제어

작성 2026-09-12. 이전 세션이 길어져 판단이 흔들렸고, 같은 결론을 두 번 도출했다.
**아래 §3이 그 결론이며, 새 세션은 §3부터 읽으면 된다.**

관련 이슈: **#5 #6 #7**(0.90), **#259**(capacity 고갈), #262, #255, #60, #101, #87

---

## 1. gRPC bench와 0.90 목표

### 1.1 비교 구조

`framework/bench/grpc/README.ko.md`가 규범이다. 언어마다 **3행**을 낸다.

| 행 | 뜻 |
|---|---|
| `grpc-<lang>` | 그 언어의 gRPC unary RPC |
| `zlink-<lang>` | framework를 거치지 않는 raw binding의 ROUTER↔ROUTER |
| `zlink-framework-<lang>` | framework RouteMesh의 RID 직접 node request/send |

`grpc-c`·`zlink-c`가 바닥 기준이다. 목표는 **framework ≥ 0.90 × 같은 언어 raw**다.

**아직 gRPC 포함 전체 비교표를 내지 못했다.** #259가 C++ request 측정을 막고 있다(§2.1).

### 1.2 2026-09-11 재측정 결과 (snapshot `00d5b25c`)

짝지어 5쌍, `framework / raw` 중앙값이다.

| 언어 | request | send |
|---|---|---|
| .NET | **1.1775 PASS** (1 KiB) | 0.0677 / 0.1185 |
| Java | **1.3033 / 1.3142 PASS** | 0.0997 |
| C++ | **측정 불가 — #259** | 0.0608 / 0.0983 |

**request는 목표를 넘겼다. framework가 raw보다 빠르다.** #12(raw 분모 수정), #47, #48이 함께 들어간 결과다.

### 1.3 send가 안 움직이는 이유 — 확인된 것

PR #265가 네 지점을 고쳤는데 **비율이 거의 그대로였다**(Java 0.086/0.141, C++ 0.062/0.105, .NET 0.084/0.148).

원자료가 이유를 보여준다.

```
raw:        peak P1 ~ P2
framework:  peak P8   ← 모든 행
```

**raw는 concurrency 1~2만 쓰고도 10배 빠르다. framework는 8을 다 쓰고도 느리다.**
파이프가 꽉 찬 상태에서도 느리므로 **깊이가 아니라 건당 비용이 지배한다.**

건당 비용 실측(`µs/message`, CPU core-seconds ÷ 메시지 수 중앙값):

| 언어 | B | source raw → framework | target raw → framework |
|---|---:|---:|---:|
| .NET | 1024 | 4.44 → 108.50 (**24.4×**) | 1.98 → 75.72 (**38.2×**) |
| Java | 1024 | 1.48 → 11.56 (**7.8×**) | 1.88 → 26.56 (**14.1×**) |
| C++ | 1024 | 1.04 → 19.43 (**18.7×**) | 1.37 → 32.01 (**23.4×**) |

**본체는 typed encode, service-wire 포장, owner turn, 공개 completion 처리다.**
0.90은 이 구조적 비용을 줄여야 달성된다. **다음 작업은 여기다.**

### 1.4 이미 들어간 것 — 중복 투입 금지

| PR | 내용 |
|---|---|
| #241 | .NET #48 9항목 중 8개 (lane batch, task wrapper 제거, native part, DI factory 등) |
| #247 | Java #47 DI 활성화 준비·재사용. p99 −35.2 %, throughput +13.3 % |
| #246 | Node #50 ingress 폴링 → readiness. 규칙 3→1 |
| #245 | Node #45 envelope 수신 fast path (Java·C++·.NET은 이미 되어 있었다) |
| #256 | Java binding #14 `sun.misc.Unsafe` → FFM. GC 93→25, allocation 37.2GB→11.8GB |
| #265 | send 4지점 — Java §15 위반, C++/Java/.NET target 중복 경계 |

**#47·#48·#50·#45·#14는 닫혔다.** 다시 열지 마라.

### 1.5 측정 방법론 — 세 번 결론을 뒤집었다

**1-run 비교와 순차 측정은 반대 결론을 만든다.**

- **#47** — 이전 보고가 "p99 0.451 → 147.369ms 회귀"로 개선 판정을 보류했다. 5쌍으로 재니
  **147ms 이상치가 변경 *전* 3회차에서 나왔다**(`peak_in_flight=1042`).
- **#143** — 순차 측정이 이상치 하나로 "+16.7 % 개선" 허상을 만들었다. 변경 전 산포가 47 %였다.
- **#14** — `SENDSEND 64B p95`가 **구현 속도가 아니라 in-flight 깊이를 재고 있었다.**
  처리량(λ) 유지, 깊이(L) 변동 → 지연(W) 변동(Little's Law). 깊이를 계측하니 p95 **−42.79 %**.

**규칙**: 짝지어 번갈아(raw 먼저, 그다음 framework), **최소 5쌍, 5개 값 전부 보고**,
`peak_in_flight`·`mean_in_flight`를 처리량·지연과 **함께** 읽는다.
**1-run 비교를 근거로 쓰지 않는다.**

### 1.6 측정 환경

- `scripts/perf/perf-ticket.sh submit`으로 제출한다. 직접 벤치를 돌리지 않는다.
- 큐는 PR #250으로 **모든 worktree가 공유**한다(`git rev-parse --git-common-dir` 기준).
  runner 중복 기동은 막힌다.
- 머신 전역 flock(`/tmp/zlink-perf.lock`)이 실행을 직렬화한다. **큐가 여러 개여도 동시 실행은 없다.**
- 측정 중 빌드 금지. 시작 전 `/proc/loadavg` 확인.

---

## 2. HWM 흐름 제어 — #259

### 2.1 증상

C++ framework **request 벤치 10/10 실패**. 0.90 판정 불가.

```
zlink::framework::framework_exception_t:
raw mesh request completion capacity is exhausted

submitted = 96,491 / completed = 8,443 / capacity 오류 = 88,048 (91 %)
peak_in_flight = 4,097
```

**timeout이 아니다.** `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1389-1392`가
registry 자리가 없으면 **네트워크로 나가기 전에 즉시 예외를 던진다.** 기다리지도, 재시도하지도 않는다.

send에도 같은 코드가 있다(같은 파일 `:1533`, `:1565`). send가 안 터지는 것은 concurrency 8이라
상한에 안 닿기 때문이지 구현이 옳아서가 아니다.

### 2.2 두 HWM은 층위가 다르다

| 층 | 무엇을 막나 | 꽉 차면 |
|---|---|---|
| **Core byte HWM** | 소켓 queue의 byte 폭증 — **마지노선** | 기다렸다 재개 |
| **Framework 4,096** | **쌓인 job 개수** — 마지노선 | **PAUSED 신호를 보내야 한다** |

---

## 3. 결론 — 스펙에 이미 다 있다. 구현이 안 되어 있다

**이것이 이 인계의 핵심이다. 이전 세션이 이 결론에 두 번 도달했다.**

### 3.1 설계 (사용자 확정)

```
job이 4,096에 쌓임
  → "나 지금 힘드니 잠시 보내지 마" 신호를 보내는 쪽으로
  → 보내는 쪽이 받아서 Core socket backpressure 설정
  → job 개수가 내려감 → 재개 신호 → 다시 보냄
```

**framework가 대기·재개를 새로 구현하는 것이 아니다. Core 기능을 그대로 쓰고 결과를 전달할 뿐이다.**
Core send timeout이 나면 그대로 전달하거나 framework 오류로 감싸면 된다.

### 3.2 스펙 위치

모든 경로는 `framework/doc/framework/common/spec/server/` 기준이다.

| 파일 | 절 | 내용 |
|---|---|---|
| `01-execution/04-application-job-queue-and-backpressure.ko.md` | **§6 Pressure 상태와 socket 제어** | pause 80 % / resume 60 % 전이. **RouteMesh ROUTER-ROUTER와 ClientServer DEALER-ROUTER에 적용** |
| 같은 파일 | **§6 마지막 불릿** | **"이 receive-flow state API가 Framework pressure와 Core send flow 사이의 유일한 runtime 제어 지점이다"** |
| 같은 파일 | **§8 Backpressure 3단계와 한도 종류** | 거절되면 기다림 → 공간 생기면 제출 → 시간 초과면 `DeadlineExceeded` |
| `00-foundation/06-framework-api.ko.md` | `:169` | Core에 주는 feedback은 receive-flow 절대 상태 `RUNNING`·`PAUSED`뿐 |
| `03-spot-actor/03-mesh-node.ko.md` | `:479` | **RouteMesh의 두 MeshNode가 PAUSED인 상황을 전제로** 계약을 쓴다 |
| `01-execution/01-submit-and-completion.ko.md` | **§11**, `:396-400` | 4,096 합계 상한과 `CapacityExceeded` 거부. **§3.5의 수정 대상** |

Core 스펙:

| 파일 | 절 | 내용 |
|---|---|---|
| `core/doc/spec/core/socket/README.ko.md` | **"수신 flow state"** `:203-221` | `RUNNING`/`PAUSED` 의미, DEALER·ROUTER만 지원, 절대 상태 |
| 같은 파일 | `:697-715`, `:1429-1434` | 상태 전달 완료 시점, reconnect 뒤 재전송 |
| `core/doc/spec/core/socket/06-dealer.ko.md` | `:107-113` | `DONTWAIT` + `BACKPRESSURED` + wait token, `ZLINK_COMPLETION_WRITABLE` 재개 |
| 같은 파일 | `:185-196` | remote PAUSE는 다음 message 경계부터 적용 |
| `core/doc/spec/core/socket/07-router.ko.md` | `:321-354` | ROUTER-ROUTER는 Completion connection 사용 |

### 3.3 Core 기능 (이미 있다)

```c
zlink_socket_set_receive_flow_state(handle, ZLINK_RECEIVE_FLOW_PAUSED | RUNNING)
  core/include/zlink/socket/api.h:194
  core/include/zlink_enum.h:186
```

- `PAUSED` = "이 socket으로 새 message를 보내지 말라" — **내가 상대에게** 거는 신호
- DEALER·ROUTER만 지원. ROUTER-ROUTER는 Completion connection 사용
- 절대 상태. 반복 설정은 no-op. reconnect 뒤 자동 재전송
- 그 외: `ZLINK_OPT_SNDTIMEO`(`zlink_enum.h:73`), `DONTWAIT`+`BACKPRESSURED`+token,
  `ZLINK_COMPLETION_WRITABLE`

### 3.4 구현 gap

| 층 | 스펙 | 현재 구현 |
|---|---|---|
| 4,096 도달 | **PAUSED 신호** | **예외를 던진다** — C++ 3곳, Java 2곳, Node 3곳, .NET 0곳 |
| 보내는 쪽 | 받아서 Core backpressure 설정 | 신호가 안 오니 계속 보냄 |
| 내려감 | RUNNING → 재개 | — |

**.NET의 `RegisterReceiveFlowSocket` 호출처는 ClientServer 둘뿐이다.**
RouteMesh에 등록이 없다 — §6이 적용 범위로 명시하는데도.

```
framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkChannelBundleFactory.cs:26   (DEALER, ClientServer client)
framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkChannelBundleFactory.cs:57   (ROUTER, ClientServer server)
framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:697
```

다른 언어의 관련 위치:

```
framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp
framework/languages/cpp/framework/src/runtime/client_server/raw_client_server_owner.cpp:213-220
framework/languages/java/.../runtime/binding/ZLinkJavaRawMeshNode.java:734
framework/languages/node/packages/framework/src/runtime/dispatch/application-job-queue.ts:213
```

**4언어 × {RouteMesh, ClientServer} 전수 확인이 아직 안 됐다.**

### 3.5 스펙 수정은 하나뿐 (사용자 지시)

**"queue 크기 제약을 host 단위로 공유한다."**

대상: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` §11(`:396-400`)과 `.en.md` 대응 절.

현행 문장은 "합친 수는 4,096개를 넘지 않는다"라고만 쓰고 **누구의 합계인지 안 쓴다.**
그래서 갈라졌다.

- C++은 process-shared dispatcher `reserved`로 센다
  (`framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:139-147`)
- **C++·.NET·Node의 ClientServer는 Framework registry를 아예 안 쓴다.** Java만 쓴다
  (`framework/languages/java/.../internal/service/ZLinkServiceOperationRegistry.java:20-31`)
- 4,096이 고정인지 설정인지도 안 쓴다
  (`framework/languages/cpp/framework/src/runtime/foundation/operation_registry.hpp:21`)

`Framework host instance`는 이미 `00-foundation/02-glossary.ko.md`에 있고 job permit도
"host-shared permit"이다(`04-application-job-queue-and-backpressure.ko.md:85`).
**같은 단위를 completion 예약에 쓰면 된다.**

### 3.6 해석의 여지를 없애야 한다 (사용자 지시)

**4언어가 제각각인 것은 구현자 탓이 아니라 조항이 여러 갈래로 읽히기 때문이다.**
스펙 구체화 시 최소한 이 셋을 명시해야 한다.

1. 합계의 **소유 단위**(host)
2. 대상이 **모든 Messaging Request**인지(RouteMesh + ClientServer)
3. 4,096이 고정인지 설정인지

---

## 4. 다음 세션이 할 일

1. **4언어 × {RouteMesh, ClientServer} 전수표**를 만든다. 각 칸에 `file:line`.
   - completion registry 사용 여부, 상한 소유 단위, receive-flow 등록 여부
   - 각 칸이 **스펙 위반인지 모호함 때문인지** 판정
2. **스펙 수정안** — §3.5의 host 단위 공유 + §3.6의 모호함 제거.
   문안 → codex 리뷰 → 사용자 승인 → 4언어 적용.
3. **구현 gap 수정** — `CapacityExceeded` 예외를 §6 흐름 제어로 대체. 4언어.
4. **#259 해소 확인** — C++ request 벤치 5쌍이 오류 없이 완주.
5. **gRPC 포함 전체 비교표** — 언어 × {grpc, raw, framework} × {request, send} × {1 KiB, 4 KiB}.
6. **send 건당 비용**(§1.3) 감축 — 0.90의 본체.

### 주의 — 이전 세션의 오판

같은 함정을 반복하지 않도록 남긴다.

- **"완료 콜백이 증발한다"** → C++ `notify`는 `noexcept`이고 반환값도 안 본다. 거부 경로가 없다.
- **"스펙 공백이다"** → 공백이 아니다. §3.2의 조항에 다 있다.
- **"재개 신호를 새로 설계해야"** → `zlink_socket_set_receive_flow_state`가 이미 있다.
- **"completion 예약을 receive-flow 입력에 넣자"** → **방향이 반대다.** completion 예약은
  **송신** 쪽이 소비하고 receive-flow는 **수신** 쪽이 상대를 멈추는 것이다.
  DEALER-ROUTER에서는 `PAUSED`가 reply까지 막아 예약 반환을 더 늦춘다.
- **"framework registry 4,096이 node별이라 8,192가 된다"** → 틀렸다. process-shared
  `reserved`가 합계를 막는다. 단 ClientServer 세 언어는 그 집계 바깥이다.

**공통 원인: 코드와 스펙을 끝까지 읽지 않고 한 조각으로 단정했다.**
결함을 진단할 때는 **소유 조항을 먼저 찾아 인용하고, 인용하지 못하면 그것이 추상 신호다.**

---

## 5. 기타 열린 이슈

| 이슈 | 내용 |
|---|---|
| **#262** | Core가 `BACKPRESSURED` 반환 시 `errno`를 `EAGAIN`으로 남기지 않는다(스펙 위반). `core/src/api/socket/socket_request_reply_submit_api.cpp:282`가 덮어쓴다. 스펙은 `core/doc/spec/core/socket/06-dealer.ko.md:411-413`. **4언어 binding이 errno를 검사하므로 전체 영향** |
| **#255** | `AutomaticTurnDispatch` E2E가 async terminal 개명을 안 따라가 빌드 실패 |
| **#60** | oversized reply가 재현되지 않는다. 증거 기록 후 열어둠 |
| **#101 · #87** | 릴리스. 0.90 판정 후 |
