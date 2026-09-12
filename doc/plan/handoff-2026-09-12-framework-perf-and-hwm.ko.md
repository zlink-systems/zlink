# 인계 — framework 성능(gRPC bench)과 HWM 흐름 제어

작성 2026-09-12. 이전 세션이 길어져 판단이 흔들렸고, 같은 결론을 두 번 도출했다.
**§3이 HWM 결론이고, §5가 남은 이슈다. 새 세션은 §5부터 읽어라.**
**#259 스펙 개정안은 `doc/plan/spec-259-completion-capacity-proposal.ko.md`에 있고 사용자 결정을 기다린다.**

관련 이슈: **#5 #6 #7**(0.90), **#259**(capacity 고갈), **#277**(Node 측정 불가), #101, #60. #276·#278·#279는 머지·해소됨.

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

## 3. 결론 — 두 상한은 별개다

> **2026-09-12 전수 조사(`hwm-gap-survey`)로 아래 §3.0을 추가했다. §3.1~§3.4의 "job queue가
> 차면 PAUSED를 못 보낸다"는 서술은 부정확했다. 실제 터지는 곳은 다른 counter다.**

### 3.0 전수 조사 결과 — 먼저 읽어라

조사 기준 commit `9f04f3ff6689`. 정적 대조만 수행했다.

| 상한 | 상태 |
|---|---|
| **Application Job Queue 4,096** | **네 언어 모두 포화 시 기다린다.** §6의 80 % PAUSE / 60 % RUNNING 전이도 **네 언어 모두 구현돼 있다.** 예외를 던지는 경로는 없다 |
| **Completion registry 4,096** | **별개 counter.** request를 보내기 전에 callback 자리를 예약하고, 없으면 §11이 명시적으로 `CapacityExceeded`를 요구한다. **여기서 터진다** |

**두 counter는 방향도 다르다.** job queue는 수신 쪽 압력이고 completion 예약은 송신 쪽 자원이다.
따라서 completion registry의 `CapacityExceeded`를 **§6 위반으로 분류할 수 없다.**

#### §6 판정 — 누락은 한 곳뿐

| 언어 | RouteMesh | ClientServer |
|---|---|---|
| C++ | 충족 | 충족 |
| Java | 충족 | 충족 |
| Node.js | 충족 | 충족 |
| **.NET** | **위반** — queue state는 계산하지만 ROUTER가 receive-flow 대상에 등록되지 않는다 | 충족 |

#### §11 판정 — 해석 공백 셋이 갈라짐을 만들었다

`01-submit-and-completion.ko.md` §11(`:396-400`)이 셋을 명시하지 않는다.

1. "합친 수"가 **registry별인지 host별인지 process별인지**
2. **모든 Messaging Request**인지 RouteMesh만인지
3. 4,096이 **고정값인지 설정값인지**

그 결과:

| 경로 | 현재 |
|---|---|
| C++·Java·.NET RouteMesh | registry별 guard + process-shared dispatcher guard를 **함께** 둔다 |
| **Java ClientServer** | 같은 구조의 completion reservation을 쓴다 — **네 언어 중 유일** |
| **C++·Node.js·.NET ClientServer** | Framework completion reservation이 **없다.** binding/Core request를 직접 기다린다 |
| **Node.js RouteMesh** | 일부 경로에 registry가 없고, 있는 것도 MeshNode별이다 |

**ClientServer 세 언어의 누락과 Node.js의 소유 단위 차이는 현행 문장만으로는 확정적 위반이
아니라 §11 해석 차이다.** 그래서 §3.5의 스펙 구체화가 필요하다.

전수표 원본: `.artifacts/codex/hwm-gap-survey/`의 job 로그(파일 저장은 읽기 전용으로 실패).

---

**아래 §3.1~§3.6은 조사 이전 서술이다. §3.5(스펙 수정)와 §3.6(모호함 제거)은 여전히 유효하고,
§3.4의 gap 표는 위 §3.0으로 대체한다.**

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

## 5. 남은 이슈 — 2026-09-12 세션 종료 시점

### 5.0 main은 green이다 — #278 해소됨

PR #270 머지 이후 main의 win-x64·win-arm64가 적색이었고, **PR #279로 고쳐 머지했다.**
`origin/main` = `560d3d8bdc` 기준 전 플랫폼 통과.

원인은 `SetLastError = true`가 캡처하는 값의 플랫폼 차이였다. Unix에서 CLR은 CRT `errno`를
스냅숏하지만 Windows에서는 Win32 `GetLastError()`를 스냅숏한다. Core는 두 플랫폼 모두 CRT
`errno`만 쓴다 — `err.cpp:223`이 `WSAEWOULDBLOCK`을 `EBUSY`로 **번역**해서 넣기 때문에
`errno`에 Winsock 원본 값이 남는 경로가 없다.

수정은 `NativeMethods.Core.cs`의 `GetLastPInvokeError()` **한 곳에서만** 분기한다.
Windows는 `zlink_errno()`를 반환 직후 즉시 호출하고, Unix는 기존 경로를 유지한다.
호출부 41곳은 그대로라 규칙 수가 늘지 않는다.

**함께 고친 것 — 테스트가 회귀를 통과시킨 이유.** 기존 단언이
`NativeErrno is 11 or 35 or 10035`였다. `10035`는 WSAEWOULDBLOCK이고 `errno`에는 들어올 수
없는 값이며 **깨진 Win32 경로에서만** 나온다. 이를 허용하는 단언이 #270의 결함을 가렸다.
`11 or 35`로 좁혔다.

**남은 같은 종류의 마스킹(후속 과제).** 제품 소스에 Win32 변형을 함께 받는 매핑이 있다 —
`ZlinkException.Native.cs:222,266`의 `11 or 35 or 10035`, `SendResultErrno.cs`의
`EWouldBlockWin`·`ENotConnWin`·`ETimedOutWin`·`EHostUnreachWin`,
`test_tokenless_backpressure.cs:57`. Core가 번역해서 넣으므로 닿지 않는 경로다. 정리 대상.

### 5.1 0.18.0 열린 이슈

| 이슈 | 상태 | 다음 행동 |
|---|---|---|
| **#259** | 조사 완료. **개정안 문서 작성됨** | `doc/plan/spec-259-completion-capacity-proposal.ko.md` §3의 ①②③을 **사용자가 결정** → codex 리뷰 → 4언어 적용 |
| **#5 #6 #7** | request PASS, send 미달 | send **건당 비용**(§1.3) 감축. 깊이 문제가 아니다 |
| **#101** | **#277에 막혀 있다** | #277 해소 후 7언어 전수 재측정 |
| **#277** | 신규 | Node REQREP가 `request completion drain timed out`으로 측정 불가 |
| **#60** | 현재 재현 안 됨 | 닫는 조건 = linux-x64·win-x64 Debug/net8.0 **연속 10회 green** |

### 5.2 열린 PR

| PR | 상태 |
|---|---|
| ~~#276~~ | **머지됨** (`560d3d8bdc`). .NET RouteMesh receive-flow 등록 — 4언어 중 유일한 §6 위반이었다. #259 본체는 닫지 않았다 |
| ~~#279~~ | **머지됨**. §5.0 |
| #275 #274 #272 | 0.19.0 Windows 계열 draft. 타 담당 |
| #226 #213 #31 | 원작업자 담당 (사용자 지시) |

### 5.3 #262에 대한 정정 — 이전 판의 기록이 틀렸다

이 문서의 이전 판은 #262를 이렇게 적었다.

> Core가 `BACKPRESSURED` 반환 시 `errno`를 `EAGAIN`으로 남기지 않는다(스펙 위반).
> `socket_request_reply_submit_api.cpp:282`가 덮어쓴다.

**틀렸다.** Core는 `socket_send_complete.cpp:169,234`에서 **명시적으로** `errno = EAGAIN`을
설정하며 git 이력이 그 의도를 확인해 준다. 실제 원인은 **.NET P/Invoke 경계에서 errno가
유실된 것**이었고, 독립 probe로 증명했다 — `SetLastError=true` 즉시 캡처 11,
별도 지연 `zlink_errno()` 호출 0.

PR #270이 그 정공법을 넣었고 #262는 닫혔다. 다만 Windows 분기를 놓쳐 #278이 생겼다.

### 5.4 이번 세션에서 정리한 것

- 실행 중이던 codex job 3건 종료. perf ticket 큐 비움.
- **worktree 94개 → 4개.** 판정 기준은 `git cherry origin/main <branch>`다.
  `git merge-base --is-ancestor`는 squash merge를 미머지로 오판한다 —
  44개로 보였던 미머지가 실제로는 29개가 이미 머지된 PR이었다.
- 남긴 worktree: `main`, `zlink-259-dotnet`(PR #276), 릴리스용 detached 2개.

### 5.5 되풀이하지 말 것 — 이번 세션의 오판

§4의 목록에 더한다.

- **"3건이 3시간째 진행 중인 건 perf 큐 락 대기 때문"** → 절반만 맞았다.
  두 perf job은 대기가 아니라 **측정 실패 루프**에 빠져 있었다(#277).
  큐 상태만 보고 job 로그의 오류를 안 읽었다.
- **"PR #276의 Windows 실패는 내 변경 탓"** → 아니다. main이 이미 적색이었다.
  **로컬 main이 3커밋 뒤처진 상태로 코드를 읽어서** #270이 안 들어간 것처럼 보였다.
  진단 전에 `git fetch && git log origin/main`을 먼저 한다.
- **PR 본문에 `Closes #259`를 썼다** → #276은 #259 본체(C++ capacity 고갈)를 고치지 않는다.
  머지되면 미해결 이슈가 자동으로 닫혔을 것이다. `Refs`로 정정했다.
  **닫기 키워드는 이슈 본문이 요구한 것을 실제로 해결했을 때만 쓴다.**
