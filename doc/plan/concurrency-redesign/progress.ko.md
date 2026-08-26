# 진행 상황 — lane 소유 동시성 전환

> 이 캠페인의 살아 있는 추적 문서다. 항목이 끝날 때마다 갱신한다.
> 규칙은 [스펙 06](../../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md),
> 순서 근거는 [conversion-order-survey.ko.md](conversion-order-survey.ko.md),
> 배경·이어받기는 [handoff.ko.md](handoff.ko.md).

브랜치 `refactor/lane-ownership-concurrency` · 시작 2026-08-26

## 0. 한눈에

> 수치는 배치 게이트(CP2)마다 갱신한다. 마지막 측정 2026-08-26.

| 지표 | 시작 | 현재 | 목표 |
|---|---:|---:|---|
| dotnet `lock` 문 | 999 | **783** (−22%) | ~60 (lane·큐 내부 구현만) |
| `_gate` 보유 클래스 | 61 | **55** | 0 |
| state lane 사용 클래스 | 0 | **7** | — |
| **async 경계 스냅샷** *(진짜 지표)* | ~465 | 미측정 | **0** |
| 호환 경계 `AwaitStateLane` *(부채)* | 0 | **58** | 0 (§3에서 회수) |

**`lock` 개수는 성공 지표가 아니다.** `SemaphoreSlim`으로 바꾸기만 해도 0이 된다. 판정은
"async 경계를 넘는 스냅샷"이 0이 되는가로 한다. 호환 경계 58은 전환의 부산물인 **부채**이며
늘어나는 것이 정상이고, §3에서 회수한다.

### 진행 단계

| 단계 | 범위 | 상태 |
|---|---|---|
| 설계·primitive | 스펙 06, `ZLinkStateLane`, golden 14 | **완료** |
| **.NET 전환** | 61클래스 | **진행 중** — 완료 3 · 진행 4 · 대기 ~54 (§2) |
| .NET 마무리 | 호환 경계 회수, posddd ②, 마일스톤 게이트 | 대기 (§3) |
| 언어 확산 | cpp · JVM · node — L0 → L1 → L2 | 진행 중 — L0 완료 · L1 착수 (§4) |
| **ZoneWorld 결함 해소 (최종)** | §8 기존 결함 3건 수정 + ZoneWorld 게이트 복귀 | 대기 (§3.1) |

### 언어별 (상세 §4)

| | dotnet | cpp | java | kotlin | node |
|---|---|---|---|---|---|
| L0 primitive | **완료** | 대기 | 대기 | 대기 | 대기 |
| L1 표본 | **완료** | 대기 | 대기 | 대기 | 대기 |
| L2 확산 | **진행 중** | 대기 | 대기 | 대기 | 대기 |

빌드 트리는 4개(dotnet·cpp·node·JVM)이고 **java와 kotlin은 JVM 트리를 공유**하므로
동시 실행 상한은 4다. L0는 언어 간·dotnet과 무의존이라 **4트리 동시 착수 가능**하다.

## 0.1 일을 어떻게 자르는가

| 단위 | 크기 | 무엇으로 끝나는가 | 누가 |
|---|---|---|---|
| **요청 1건** | 클래스 1개 (대형은 2개 묶음) | 에이전트 보고 (CP1) | codex 1잡 |
| **커밋 1건** | 요청 1건 | 리뷰 승인 후 커밋 | Claude |
| **배치** | 2~4 요청 | 전체 unit + 6샘플 게이트 → 푸시 (CP2) | Claude |
| **마일스톤** | .NET 전체 | §3 전 항목 (CP3) | Claude |

**병렬은 요청 단위로만 한다.** 서로 다른 파일을 만지는 요청은 동시에 띄우되, 빌드·테스트는
트리 락으로 직렬화한다(§4). 게이트와 커밋은 항상 중앙에서 한 번씩이다.

## 1. 진행 방식 (확정)

- **역할**: 전환 = codex(표본급 난이도만 sol, 이후 terra) · 감독/리뷰/판정/커밋 = Claude.
- **2패스 원칙**: ①동작 보존 lane 전환(본문 무변경) → 커밋 → ②posddd 리팩토링
  (할당·복사·경합·죽은 코드, `doc/principal/dev/posddd.ko.md`) → 별도 커밋.
  ②는 측정 우선순위가 낮으면 근거를 남기고 생략할 수 있다.
- **가속 모드**(2026-08-26 사용자 승인): 서로 다른 파일을 만지는 클래스는 codex 잡을
  병렬로 돌린다. 같은 트리의 빌드·테스트는 `flock /tmp/zlink-dotnet-gate.lock`으로
  직렬화한다. 전체 unit 게이트와 6샘플 게이트는 배치(2~4클래스)마다 중앙(Claude) 1회.
  기계 변환 diff는 금지 패턴 grep+표본 검토, 판단이 들어간 변환과 대형 2건은 전문 정독.
- **게이트**: 스펙 06 §10 — 단위 테스트 실패 0, 6샘플 placement marker 유지
  (ZoneWorld는 기존 결함으로 제외), 관측 동작 불변.
- **푸시**: 2~3클래스 누적마다.

## 2. .NET 전환 현황

상태 값은 다섯이다. **`요청됨`이 있어야 새 세션이 "이미 띄웠나"를 알 수 있다.**

`대기` → `요청됨`(잡 실행 중) → `보고됨`(CP1 대기) → `완료`(리뷰 승인·커밋) / `반려`(재요청)


| # | 클래스 | lock | 상태 | 커밋 | 비고 |
|---|---|---:|---|---|---|
| 표본 | ZLinkSessionActorBindingTable | 30 | **완료** | `3cc6f5f615` | sol. 재진입 2, 호환 경계 25(부채) |
| 1 | ZLinkInMemoryLocationStore | 27 | **완료** | `2bdf463ee3` | 재진입 0, posddd ② 생략(근거 커밋에) |
| 2 | ZLinkActorHandoffAdmissions | 25 | **완료** | `326133ca03` | SuppressFlow 1, 호환 경계 4 |
| 3 | ZLinkActorOwnershipCoordinator | 26 | **완료** | `e1f5ade349` | 재진입 8 해소(순차 await+lane 내부화), _disposeStartGate=C3, 호환 경계 3. reconciliation 시작 Task.Yield 보강(§7-4) |
| 4 | ZLinkSpotNodeCatalog | 48 | **완료** | `0fef22fa62` | SuppressFlow 5, 호환 경계 9(범위 밖 호출자 2 포함). dispose 재진입 fix `1bd23dc1ff` |
| 5·6 | ZLinkClientServerClientRuntime + Connection | 20+44 | **완료** | `39d71acd6c` | lane 2개(소유 분리), SuppressFlow 7, 재진입 0, 호환 경계 4, _socketLifecycleGate 유지 |

CP2 배치 1(#3·#4·#5·6) 게이트 2026-08-26 통과 — unit 1893/0, 7샘플 러너 exit 0.
| 7 | ZLinkActorHandoffState | 58 | **요청됨** | | codex terra 실행 중 (2026-08-26). 2× 난이도 |
| 8 | ZLinkActorRuntimeState | 35 | 대기 | | semaphore + 재진입 ~20 |
| 9 | ZLinkManagedMeshNode | 130 | 대기 | | 4×+, 정독 리뷰 필수 |
| 10 | ZLinkFrameworkRuntime | 27 | 대기(최종) | | 사용 45파일, 재진입 ~57, 정독 리뷰 필수 |
| — | 소형 잔여 (~50클래스, lock<20) | ~300 | 대기 | | 3~5개 배치, C1/C3 다수 예상 |
| 제외 | ZLinkSerialExecutionQueue | 25 | 제외 | | lane primitive 자체 (스펙 06 §9) |

## 3. .NET 마무리 항목 (클래스 전환 후)

- [ ] **호환 경계 제거** — `AwaitStateLane` 잔여(표본 25 + 이후 발생분)를 async 전파로
  해소. 남는 게 있으면 사유 기록.
- [ ] **posddd ②패스 잔여** — survey의 관찰 목록 기준. 최우선: `ZLinkFrameworkRuntime`
  hot path의 `ZLinkRuntimeOperationLease` class 할당.
- [ ] **마일스톤 게이트** — 전체 unit + 6샘플 + cross-language harness 재확인,
  "async 경계 스냅샷" 계열 재측정, **§8 기존 결함 3건 재확인**(cpp ZoneWorld
  split-brain / dotnet ZoneWorld mesh admission / fast_mutex abort — 이 작업의 하위
  증상인지 판정), **codex sol 마일스톤 리뷰**, 스펙 06 보완 필요 여부 판정.
- [ ] **ZoneWorld 결함 해소 (§3.1)** — 판정 후 남는 결함을 직접 수정하고 ZoneWorld를
  게이트에 복귀시킨다. 캠페인의 최종 단계다.

### 3.1 ZoneWorld 결함 해소 — 캠페인 최종 단계 (2026-08-26 사용자 지시)

CP3 판정("이 작업의 하위 증상인가")에서 **끝내지 않는다.** 판정 결과와 무관하게, 남아 있는
결함을 직접 수정해 ZoneWorld를 게이트에 복귀시키는 것까지가 이 캠페인의 완료 조건이다.
진입 조건: dotnet 마일스톤 게이트(§3) 통과 후. lane 전환으로 자연 해소된 항목은 재현
불가 확인(반복 실행)으로 닫는다.

| # | 결함 | 언어 | 상태 | 비고 |
|---|---|---|---|---|
| Z1 | ZoneWorld 세션 split-brain (서버는 죽었다고, client는 살았다고 봄) | cpp | 대기 | 재현: 7샘플 일괄. 종료 경로 3개 중 1개는 기수정 |
| Z2 | ZoneWorld mesh admission `Hello` 무한 반복 | dotnet | 대기 | 직접 3회 실행 0/3. 과거 codex 시도는 unit 깨서 되돌림 |
| Z3 | `fast_mutex.hpp:76` abort (EINVAL on unlock = 소유 객체 파괴) | cpp | 대기 | Z1과 같은 계열 추정. cpp L1/L2 전환에서 단서 기대 |
| Z4 | ZoneWorld를 6샘플 제외 목록에서 빼고 7샘플 전부를 게이트로 복귀 | 전 언어 | 대기 | Z1~Z3 해소 후. 30런급 반복으로 flake 아님 확인 |

## 4. 언어 병렬 단계

.NET이 규칙의 정본이다(정본-우선 포팅 정책). 다른 언어는 .NET에서 확정된 규칙을 옮기며,
**단계마다 진입 조건이 있고 조건을 만족하면 언어끼리는 서로 기다리지 않고 병렬로 간다.**

### 빌드 트리는 4개뿐이다

| 트리 | 언어 | 게이트 락 |
|---|---|---|
| dotnet | dotnet | `flock /tmp/zlink-dotnet-gate.lock` |
| cpp | cpp | `flock /tmp/zlink-cpp-gate.lock` |
| node | node | `flock /tmp/zlink-node-gate.lock` |
| JVM | **java + kotlin 공유** | `flock /tmp/zlink-jvm-gate.lock` |

**java와 kotlin은 절대 동시에 빌드하지 않는다.** Gradle 트리를 공유해서 동시 빌드가 무관한
모듈에서 `Unresolved reference` 컴파일 오류를 낸다(캠페인에서 실제로 겪음). 두 언어는 같은
락을 쓰고 순차로 돈다.

서로 다른 트리는 파일이 겹치지 않으므로 자유롭게 병렬이다. 즉 **동시 실행 상한은 4**다.

### 단계

**L0. primitive 포팅** — 진입 조건: 스펙 06 확정 (**충족**).

각 언어에 state lane primitive와 검증 테스트를 만든다. **golden은 .NET `StateLaneTests`
14개**이며, 언어별 관용구로 옮기되 검증하는 사실은 바꾸지 않는다. dotnet 전환 작업과
의존이 없으므로 **지금 바로 4트리 병렬 가능**하다.

언어별 대응 원시타입:

| 언어 | mailbox | 스케줄 플래그 | 소유 상태 |
|---|---|---|---|
| cpp | `moodycamel`/lock-free queue 또는 mutex 최소 큐 | `std::atomic<int>` CAS | 평범한 `std::map` |
| java/kotlin | `ConcurrentLinkedQueue` | `AtomicInteger` CAS | 평범한 `HashMap` |
| node | 배열 큐 (단일 스레드) | 불필요 — **재진입 검출만** 이식 | 평범한 `Map` |

**node 주의:** 단일 스레드라 상호 배제가 필요 없다. 그래서 lane의 가치가 다르다 —
"동시 접근 방지"가 아니라 **"await 사이에 끼어드는 재진입 방지"**다. golden 14개 중
동시성 항목은 의미가 없으므로 이식 대상에서 빼고, 순서·재진입·배치·종료 항목만 옮긴다.
빼는 이유를 테스트 파일에 남긴다.

**L1. 언어별 표본 1개** — 진입 조건: 해당 언어 L0 완료 + dotnet 중형 구간(§2 #5·#6) 완료.

각 언어에서 `ZLinkSessionActorBindingTable` 대응부를 찾아 전환한다. 목적은 진도가 아니라
**그 언어에서만 나오는 함정을 조기에 드러내는 것**이다. 예상되는 것:

- **cpp** — `recursive_mutex`가 있다는 것은 재진입이 실재한다는 뜻이다. 가장 많이 나올 것이다.
  또한 lane 밖으로 나가는 `shared_ptr` 스냅샷이 수명 문제(`fast_mutex` abort 계열)와
  얽혀 있어, 여기서 §8-3 판정의 단서가 나올 수 있다.
- **java/kotlin** — `CompletableFuture` 체인이 lane 경계를 넘는 지점.
- **node** — `await` 사이 재진입.

**L2. 전면 확산** — 진입 조건: 해당 언어 L1 완료 + **dotnet 마일스톤 게이트 통과**(§3).

.NET에서 확정된 클래스 목록·순서·규칙을 각 언어의 대응부에 적용한다. 언어별로
`conversion-order-survey`에 해당하는 파급 실측을 먼저 하고 순서를 정한다.

### 언어별 진행표

`대기` → `요청됨` → `보고됨` → `완료` / `반려` (§2와 같은 상태 값)

| 언어 | 트리 | L0 primitive | L1 표본 | L2 확산 | 마지막 게이트 |
|---|---|---|---|---|---|
| **dotnet** | dotnet | **완료** (`ZLinkStateLane` + golden 14) | **완료** (`3cc6f5f615`) | **진행 중** — §2 | unit 실패 0 · 6샘플 OK |
| cpp | cpp | **완료** (golden 14/14, close·try_post 재진입 가드는 정본보다 엄격) | **완료** (stream_session_registry_t 31→0, 시그니처 무변경, admit_inbound 대기 lane 밖 분리+세대화, 반려 1회: notify_changed lost wakeup) | 대기 | unit·contract 그린 (layout_contract 제외 — 기존) |
| java | JVM | **완료** (§7-5 보완 포함 — completeAsync·internal 패키지 이동) | **완료** (ZLinkSessionActorsRuntime 22→0, 인터페이스 불변·kotlin 파급 0) | 대기 | core 1,157 중 알려진 flake 1 |
| kotlin | JVM | **완료** (golden 14/14, CoroutineContext.Element 전파) | 대기 (java 조사: kotlin 파급 0 — 인터페이스 유지 시) | 대기 | kotlin 67/0 |
| node | node | **완료** (golden 12 이식 + 동시성 2 제외, 사유 주석) | **요청됨** (ZLinkActorSessionBindingRegistry, 조사 l1-survey-node.ko.md) | 대기 | state-lane 12/12 |

**L0 4건은 서로도, dotnet과도 의존이 없다.** 지금 동시에 띄울 수 있다. 다만 java와 kotlin은
같은 JVM 락을 쓰므로 빌드·테스트 구간에서만 순차가 된다.

### 게이트 매트릭스

**게이트는 언제나 Claude가 중앙에서 돌린다**(§5.1 CP2). 에이전트에게 맡기지 않는다 — 이
캠페인에서 에이전트 보고가 여러 번 사실과 달랐다.

| 언어 | 단위·계약 | 7샘플 일괄 | 비고 |
|---|---|---|---|
| dotnet | `dotnet test tests/Zlink.Framework.UnitTests` | `framework/languages/dotnet/samples/run_samples.sh` | 기준 1879+ / 실패 0 |
| cpp | `ctest` (`tests/Zlink.Framework.ContractTests`) | `framework/languages/cpp/samples/run_samples.sh` | 알려진 flake: Bingo 후반 ~1/5, TTT teardown ~1/15 |
| java | `./gradlew :zlink-framework-core:test` | `framework/languages/java/samples/run_samples.sh` | 러너가 java→kotlin **순차** 실행 |
| kotlin | `./gradlew :zlink-framework-kotlin:test` | (위와 같은 러너) | `ZLINK_SAMPLE_LANGUAGES`로 분리 가능 |
| node | `npx tsc -b tsconfig.build.json --force` 후 `node --test test/contract/*.test.js` | `framework/languages/node/samples/run_samples.sh` | **`--force` 필수** — dist가 src보다 최신이면 옛 dist로 돈다 |

**cross-language e2e**: `framework/languages/cpp/cross-language/run_cross_language_smoke.sh`
(`ZLINK_CPP_BUILD_DIR=../build`). 트리를 가로지르므로 **모든 언어 락을 잡고 단독으로** 돌린다.
CP3 마일스톤 전용이다.

#### 어느 체크포인트에서 무엇을 도는가

| | CP1 (요청 1건) | CP2 (배치 2~4) | CP3 (마일스톤) |
|---|---|---|---|
| 대상 클래스 집중 테스트 | 에이전트 | — | — |
| 해당 언어 단위·계약 | — | **Claude** | Claude |
| 해당 언어 7샘플 | — | **Claude** | Claude |
| cross-language e2e | — | — | **Claude** |
| async 경계 스냅샷 재측정 | — | — | **Claude** |

**ZoneWorld는 게이트에서 제외한다.** cpp·dotnet에 이 작업 이전부터 있던 결함 2건이 있고
(§8), CP3에서 "이 작업의 하위 증상인지" 판정한다. 그 전까지 ZoneWorld 실패를 회귀로 읽지
않는다.

**간헐 실패는 알려진 flake 목록과 대조하기 전에 회귀로도 flake로도 단정하지 않는다**(§5.1 STOP).

**cpp `test_cpp_framework_layout_contract`는 기존 실패다** (2026-08-26 확인):
`samples/ShoppingMall/Server/OrderWorkflow/main.cpp` L350·L446의 blocking `result()` 지문 —
base `3cbfbde4f9` 이후 무변경 파일이라 이 캠페인과 무관. 게이트 판정에서 제외하고,
샘플 수정은 별도 작업으로 넘긴다.

### 병렬 편성 규칙

- **한 트리에 codex 잡 여러 개**는 서로 다른 파일을 만질 때만. 빌드·테스트는 트리 락으로 직렬화.
- **cpp는 codex 안전 필터가 메모리 수명 주제를 거부**한 전례가 있다(캠페인에서 2회).
  거부되면 Claude 서브에이전트로 우회한다.
- **L0는 언어 간 무의존**이라 4트리 동시 착수 가능. L1·L2는 각 언어의 앞 단계에만 의존하고
  다른 언어를 기다리지 않는다.

## 5. 작업 주기 — 요청 → 보고 → 리뷰

전환은 에이전트가, **판정·커밋은 Claude가** 한다(§1). 그 사이를 잇는 규칙이다.

### 5.0 요청 템플릿

**아래 공통 규칙 블록을 그대로 쓴다.** 매번 새로 쓰면 빠지는 항목이 생기고, 이 캠페인에서
검증 없이 변경만 남기거나 검증을 약화시킨 사례가 실제로 있었다.

```
# 공통 규칙 (state lane 전환)

작업 디렉터리: /home/hep7/project/zlink (브랜치 refactor/lane-ownership-concurrency)

먼저 읽기:
  ① framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md (규칙 스펙)
  ② doc/plan/concurrency-redesign/sample-conversion-report.ko.md (함정: lane 안에서 시작한
     장기 작업의 AsyncLocal 소유권 상속 → ExecutionContext.SuppressFlow 처방, out 실패부가값 함정)
  ③ doc/plan/concurrency-redesign/conversion-order-survey.ko.md 의 해당 클래스 절
  ④ Runtime/Execution/ZLinkStateLane.cs

절차: 재진입 자리를 private 분리 → lock 제거·본문을 _lane.RunAsync로 감싸기(본문 로직 한
글자도 불변) → lane 안에서 시작하는 장기 작업(timer/expiry/loop)은 시작점에 SuppressFlow →
out은 스펙 06 §7대로 반환 타입화(실패 시 부가값 있으면 결과 타입 보존) → 호출자 전파(완전
전파가 허용 파일 밖으로 번지면 AwaitStateLane 호환 경계:
private static T AwaitStateLane<T>(ValueTask<T> op) => op.GetAwaiter().GetResult(); 개수 보고)
→ 직접 단위 테스트 시그니처 추종(assertion 의미 불변).

금지: ConcurrentDictionary/SemaphoreSlim 치환, posddd 리팩토링(별도 패스), spec/** 수정,
git 명령 전부, 관측 동작 변경, 허용 파일 밖 수정.

빌드·테스트 직렬화 (병렬 잡이 같은 트리를 쓰므로 필수):
  flock -w 7200 /tmp/zlink-<tree>-gate.lock <build/test 명령>
전체 게이트는 마지막에 1회만, 실패 0 확인 후 완료 선언.

보고: 재진입 실측·SuppressFlow 지점 / 시그니처·전파·호환 경계 수 / 본문 조정 목록(없으면
없음) / 테스트 결과 / 예상과 달랐던 점 / 걸린 시간.

# 대상: <클래스명>

<파일 경로>
(lock <n>, <분류> — <근거>. 조사: <장기 작업/재진입/특이점>. 호출자: <파일 목록>.)

허용 파일: 대상, 위 호출자 <n>파일, 해당 단위 테스트.
```

**대상 절의 내용은 지어내지 않는다.** `conversion-order-survey.ko.md`의 해당 클래스 절에서
옮긴다. 조사가 없으면 조사부터 한다 — 조사 없이 띄우면 허용 파일을 못 정하고 범위가 샌다.

### 5.1 보고 시점

보고 시점은 셋이고, 각각 리뷰 깊이와 산출이 다르다.

#### CP1 — 클래스 하나 완료

에이전트가 아래를 **반드시 포함해** 보고한다. 빠지면 리뷰하지 않고 반려한다.

| 항목 | 왜 필요한가 |
|---|---|
| 변경 파일 목록 | 범위 이탈 확인 |
| `lock` 전/후 개수 | |
| **재진입 몇 곳, 어떻게 해소했나** | 남은 클래스 난이도 추정의 근거 |
| **호환 경계(`AwaitStateLane`) 몇 곳, 사유** | 부채. §3에서 회수 |
| 단위 테스트 결과 (숫자로) | |
| 스펙 06 §6 유형 외의 새 유형을 만났나 | §7 후보 |
| 걸린 시간 | 추정 재산정 |

Claude 리뷰:
- **기계 변환**(본문 무변경) → 금지 패턴 grep + 표본 정독
- **판단이 들어간 변환**(재진입 해소, 호환 경계 신설) → 해당 구간 전문 정독
- 금지 패턴: lane 밖 스냅샷 잔존, `.Result`/`.Wait()` 신규, `ConcurrentDictionary`로
  C2 치환, 관측 동작 변경(순서·타임아웃·오류 코드)

산출: **승인 → §2 표 갱신 + 커밋**, 또는 **반려 → 구체 지시와 함께 재작업**.

#### CP2 — 배치(2~4클래스) 완료

- Claude가 **전체 unit 게이트 + 6샘플 게이트**를 중앙에서 1회 실행한다(에이전트에게 맡기지
  않는다 — 이 세션에서 에이전트 보고가 여러 번 사실과 달랐다).
- 통과 → 푸시. 실패 → 배치 안에서 이분 탐색으로 원인 클래스를 가른다.
- §2 표와 §7을 갱신한다.

#### CP3 — 마일스톤 (§3 항목 전부)

- 전체 unit + 6샘플 + cross-language harness
- **"async 경계 스냅샷" 재측정** — 진짜 지표. lock 개수가 아니다
- §8 기존 결함 3건이 이 작업으로 해소됐는지 판정
- codex sol 마일스톤 리뷰 + 스펙 06 보완 필요 여부 판정

#### 즉시 중단(STOP) 조건

아래는 진행하지 말고 Claude에게 올린다.

- 스펙이 정한 관측 가능한 동작을 바꿔야만 통과되는 경우 → **스펙 문제일 수 있다. 임의 변경 금지**
- 재진입이 구조적이라 lane 분할이나 경계 재설계가 필요한 경우 → 설계 §8 "경계를 새로 긋지
  않는다"에 걸린다
- 게이트가 간헐 실패 → 알려진 flake 목록과 대조 전에는 회귀로도 flake로도 단정하지 않는다

## 6. 기록 규칙

**이 문서는 진행 상황을 읽는 창구다.** 갱신을 미루면 표가 거짓말을 하므로, 아래 시점에
**해당 작업의 커밋과 같은 커밋에서** 갱신한다. 문서만 따로 나중에 고치지 않는다.

| 시점 | 갱신할 곳 | 무엇을 |
|---|---|---|
| 잡을 띄울 때 | §2 / §4 언어표 | 상태 `대기` → **`요청됨`** (중복 요청 방지) |
| 보고를 받을 때 | §2 / §4 언어표 | `요청됨` → **`보고됨`** |
| CP1 승인·커밋 | §2 / §4 언어표 | `보고됨` → **`완료`** + 커밋 해시·비고(재진입·호환 경계 수) |
| CP1 반려 | §2 / §4 언어표 | `보고됨` → **`반려`** + 반려 사유 한 줄 |
| **CP2 배치 게이트** | **§0 지표 표** | lock·`_gate`·lane 클래스·호환 경계 **재측정**, 측정 날짜 갱신 |
| CP2 배치 게이트 | §4 언어별 진행표 | 해당 언어 "마지막 게이트" 칸 |
| CP3 마일스톤 | §0 전부 + §3 | async 경계 스냅샷 측정값 기입, 단계 표 갱신 |
| 아무 때나 | §7 | 규칙이 바뀌는 발견(새 재진입 유형, 호환 경계 판단 등) |

§7의 발견은 모아 두었다가 **스펙 06 개정이 필요하면 마일스톤에서 일괄 처리**한다. 발견할
때마다 스펙을 고치면 전환 중인 클래스들이 서로 다른 판을 보게 된다.

### §0 지표 재측정 명령

CP2마다 이 네 줄을 돌려 §0 표를 갱신한다.

```bash
cd framework/languages/dotnet/src/Zlink.Framework
grep -rhoE 'lock *\(' --include=*.cs . | wc -l                        # lock 총계
grep -rhoE 'private readonly object _gate' --include=*.cs . | wc -l    # _gate 보유 클래스
grep -rlE 'ZLinkStateLane' --include=*.cs . | wc -l                    # lane 사용 클래스
grep -rhoE 'AwaitStateLane' --include=*.cs . | wc -l                   # 호환 경계(부채)
```

## 7. 전환 중 발견 (스펙 후보)

- (표본) lane 안에서 시작한 timeout 작업의 AsyncLocal 소유권 상속 → SuppressFlow 처방.
  스펙 06 §6 유형 ②로 반영 완료.
- (2번) `_operationGate` 안에서 lane 블로킹 대기 시 데드락 판정 기준: lane 큐 항목이
  그 gate를 재획득하지 않고, 완료 신호 TCS가 전부 `RunContinuationsAsynchronously`면
  안전. 호환 경계 리뷰 체크리스트로 사용.
- **(4번, 스펙 06 §6 유형② 보완 필요)** `SuppressFlow`는 **스레드 전환 이후에만** 유효하다.
  lane turn 안에서 시작한 async 메서드의 **동기 프리픽스**는 여전히 lane 위에서 실행되므로,
  프리픽스가 `_lane.RunAsync`에 도달하면 SuppressFlow가 있어도 재진입 예외가 터진다
  (`ZLinkSpotNodeCatalog.DisposeCoreAsync`에서 실측 — unit 128개 연쇄 실패). 처방:
  `Task.Run`으로 시작 자체를 lane 밖 스레드로 밀어낸다. 프리픽스가 await로 먼저 양보하는
  경우(Task.Delay로 시작하는 timeout 등)만 SuppressFlow 단독으로 충분하다. **CP1 리뷰
  체크리스트에 추가**: lane 안 장기 작업 시작점마다 "동기 프리픽스가 lane에 재진입하는가"를
  확인한다. 이 결함은 CP2 중앙 게이트(병렬 이웃 잡의 전체 실행)가 잡아냈다 — 배치 게이트를
  생략하지 말 것의 실증.
- **(5번, java L0 결함 — L1 조사가 발견)** 정본은 완료 TCS를 전부
  `RunContinuationsAsynchronously`로 만들어 caller continuation이 lane 스레드에서 inline
  실행되지 않는다. java `CompletableFuture.complete`는 비동기 아닌 dependent를 완료 스레드에서
  inline 실행하므로, CURRENT ThreadLocal 스코프 안에서 complete하면 caller의 thenApply가
  lane 표식을 물려받아 **거짓 재진입 예외**가 난다. 처방: 완료를 CURRENT 스코프 밖으로 뺀다.
  kotlin(awaiter 자기 문맥 재개)·node(microtask, resolver 문맥 비전파)·cpp(future)는 해당 없음.
  L0 포팅 리뷰 체크리스트: "완료 신호의 continuation이 어느 문맥에서 실행되는가"를 언어마다 확인.
- **(6번, 스펙 06 §6 유형 ③ 후보 — #7에서 실측)** **lock 안에서 외부 콜백을 호출하던 자리.**
  원본이 `lock (_gate)` 안에서 호출자 제공 콜백(reserveReplay)을 불렀고, 콜백이 같은 컴포넌트의
  public 표면(TryCapture)을 다시 불러도 C# Monitor 재진입 허용으로 동작했다. lane은 이를
  금지하므로 콜백 호출을 turn 밖으로 뺀다: turn A(검증+대상 산출+placeholder claim으로 이중
  예약 방지) → 콜백 실행(turn 밖) → turn B(기록·전이). 유형 ①과 달리 콜백은 외부 코드라
  private core 분리로 해소할 수 없다. 마일스톤에서 스펙 06 §6에 유형 ③으로 승격.
