# Codex 4개 runtime 통합 계획 리뷰

**대상**: `framework/doc/plan/codex-framework-internals-four-language-convergence-plan.ko.md`
**리뷰**: Claude
**결론**: **이 문서를 기준으로 진행하는 데 동의한다.** 단 IC-01·IC-02는 착수 전 정리 필요

---

## 0. 리뷰어의 사전 고지

리뷰 전에 밝혀 둘 것이 있다. 같은 주제로 내가 작성했던
`claude-language-discretion-removal.ko.md`는 **조사 여섯 항목 중 다섯이 틀려 폐기했다.**
원인은 하나였다 — **이름만 grep해서 표를 만들고 계층을 확인하지 않았다.**

내가 틀린 목록:

| 내가 적은 것 | 실제 |
|---|---|
| `isReady` 개수로 "네 runtime 모두 참·거짓 하나로 관리" | 네 runtime 모두 일곱 상태 enum 보유 |
| ".NET·C++은 Spot 전용 타입 없음" | 네 runtime 모두 있음. 없는 건 `UserSpot`뿐 |
| "Node만 `infrastructure`" | java에도 `ZLinkServiceMailbox.Domain.INFRASTRUCTURE` 존재 |
| "Node만 잠금 없음" (개수 grep) | 개수는 무관. 네 runtime 모두 확인·삽입이 한 함수 |
| **"Node에 실행 직렬화 계층 없음"** | **`runtime/execution/serial-scheduler.ts` 존재 — Codex EV-04가 맞았다** |

마지막 항목은 재조사한 뒤에도 틀린 것이다. 그래서 이 리뷰는 **Codex 문서의 근거를 그대로
믿지 않고 직접 검증한 결과**를 기반으로 한다.

---

## 1. 근거 검증 결과

EV 항목 중 수치 근거를 직접 확인했다. **전부 정확했다.**

| 근거 | 문서 주장 | 검증 |
|---|---|---|
| EV-01 | C++ app 1,024/64MiB, control 128/4MiB, 10ms, burst 8, fixed 256 | `dispatch_limits.hpp` 일치 |
| EV-02 | JVM이 C++과 같은 기본값 | `ZLinkAsyncSerialQueue` 일치 |
| EV-03 | .NET 4,096/256건, burst 32, 단일 admission gate | `ZLinkSerialExecutionQueue` 일치 |
| EV-04 | Node 4,096/1,024건, app 16MiB, 50ms | `serial-scheduler.ts` 일치 |

직접 확인한 네 runtime 실제 값:

| runtime | app 개수 | app byte | lifecycle 개수 | owner turn | burst |
|---|---|---|---|---|---|
| C++ | 1,024 | 64 MiB | 128 | 10 ms | 8 |
| JVM | 1,024 | 64 MiB | 128 | 10 ms | 8 |
| .NET | **4,096** | 64 MiB | **256** | 10 ms | **32** |
| Node | **4,096** | **16 MiB** | **1,024** | **50 ms** | 8 |

### 1.1 사소한 누락

6.1 「현재 차이」의 .NET 항목에 **owner turn budget이 빠져 있다.** 실제로는
`OwnerTimeSliceMilliseconds = 10`으로 C++/JVM과 같다
(`ZLinkSerialExecutionQueue.cs:12`). 지금 서술은 .NET의 개수와 burst만 적고 있어,
**"owner turn을 50ms로 쓰는 것은 Node 하나뿐"**이라는 사실이 드러나지 않는다.

---

## 2. 강점

### 2.1 근거를 파일·행으로 고정했다

EV-01~EV-20이 모두 `경로:행` 형식이다. 내 문서가 무너진 직접 원인이 근거 부재였으므로,
이 차이가 두 문서의 신뢰도를 가른다.

### 2.2 검증 수준을 판정에 포함했다

`MATCH` / `LANGUAGE-MAPPING` / `DIVERGED` / `UNVERIFIED` 네 값으로 **"확인했다"와 "이름만
봤다"를 구분한다.** 7장의 다음 문장이 특히 중요하다.

> 문서에 "한 구현은"이라고 적힌 역사적 설명은 source 확인 전에는 현재 사실로 사용하지
> 않는다.

내가 정확히 이 함정에 빠졌다. `10-liveness-and-state`의 "한 구현은 전역 참·거짓 값
하나로 관리한다"를 현재 사실로 읽고 조사를 시작했는데, 확인해 보니 해당하는 runtime이
없었다. 7.2의 freshness audit(`CURRENT`/`FIXED-HISTORY`/`STALE`/`UNVERIFIED`)은 이
문제를 구조적으로 막는다.

### 2.3 내 제안의 기각 근거가 타당하다

2.3 대조표에서 내 제안 다섯을 채택하고 넷을 기각했는데, **기각이 모두 옳다.**

| 기각한 내 제안 | Codex 근거 | 내 평가 |
|---|---|---|
| Java `binding`을 `backend`로 즉시 rename | JNI adapter와 raw backend port를 같은 책임으로 입증하기 전에는 합치지 않음 | **옳다.** 나는 디렉터리 파일 수만 보고 "이름만 다르다"고 판단했다 |
| Node `infrastructure`를 `lifecycle`로 즉시 rename | execution area와 serial work lane은 별도 bounded context | **옳다.** 나는 서로 다른 계층을 비교했다 |
| 다수 구현의 이름·구조를 그대로 채택 | 다수결보다 authority·불변식·자원 bound 기준 | **옳다.** 다수결은 근거가 아니다 |
| 재량 범주 전체 제거 | trace thunk, mutex/event loop처럼 결과가 같은 mapping은 유지 | **옳다.** 언어에 없는 장치는 강제할 수 없다 |
| 보호 문서와 네 구현을 항상 한 commit | spec-first commit 뒤 gap ledger + 언어별 checkpoint | **옳다.** 네 runtime을 한 commit에 넣으면 회귀 원인 분리가 불가능하다 |

### 2.4 내가 놓친 영역을 찾아냈다

내 계획은 internals의 재량 표기 여섯 곳만 봤다. Codex는 실제 조사로 다음을 추가로 찾았다.

- serial limit 수치가 runtime마다 다름 (EV-01~04)
- Node의 같은 owner inline reentrancy (EV-05)
- observation 자원 모델 차이 — C++ observer별 thread (EV-06)
- codec 선택 의미, payload ownership, completion terminal winner

---

## 3. 문제점

### 3.1 IC-01 수치 선택에 근거가 없다 — **착수 전 정리 필요**

application 한도가 C++/JVM 1,024 대 .NET/Node 4,096으로 **2:2로 갈린다.** 문서는 1,024을
확정했으나 6.1의 근거는 다음이 전부다.

> C++·JVM의 논리 값과 no-inline 규칙을 선택하고 실행 자원은 process-wide로 주입

**왜 작은 쪽인가**가 없다. 4장이 스스로 이렇게 선언했으므로 그 기준에 따른 근거가 있어야
한다.

> 통합안은 다수결로 선택하지 않는다. … authority·불변식·자원 bound를 기준으로 선택한다.

처리량 상한을 4배 줄이는 변경이고, 14장이 "수치 통일이 기존 공개 SLA 또는 memory bound를
악화시킴"을 중단 조건으로 두고 있다. **중단 조건이 걸릴 수 있는 결정이 근거 없이
확정되어 있다.**

필요한 것: 1,024과 4,096 중 어느 쪽이 어떤 불변식·자원 bound를 만족시키는지, 큰 값을
쓰는 두 runtime의 처리량이 줄어드는 영향은 어떻게 판단했는지.

### 3.2 조사 전에 확정한 항목이 넷 — **표기 정리 필요**

IC-04, IC-08, IC-10, IC-14는 결정이 `CONFIRMED`인데 현재 구현 상태가 `UNVERIFIED`다.
즉 **현재 어떤지 모르는 채 목표를 고정했다.**

3장은 두 축을 분리한다고 설명한다.

> 이 문서의 canonical 결정은 모두 확정되어 있으며, `UNVERIFIED`는 구현 call path 증거가
> 부족하다는 뜻으로만 사용한다.

설명은 이해하지만, WP1 조사 결과에 따라 IC 자체를 되돌려야 할 수 있다. 5장은 반대로
못박는다.

> source audit에서 새 차이가 나오면 구현 gap을 추가할 뿐, 각 언어 구현자가 다른 정책을
> 다시 고르지 않는다.

이 구조는 내가 이번에 다섯 번 틀린 것과 같다 — **조사 전 단정.** 두 진술이 긴장하므로,
`UNVERIFIED` 상태인 IC는 "WP1 결과에 따라 재확정 가능"임을 명시하는 편이 안전하다.

### 3.3 조사 대상이라던 것을 이미 확정했다 — **표기 정리 필요**

2.3 대조표는 조사 항목으로 둔다.

> C++ `host_runtime_state_t`와 Node discovery 상태의 정체 조사 | **채택**

그런데 6.9와 WP5(C++)에서는 이미 결론을 확정했다.

> `host_runtime_state_t`를 `maintenance_admission_state_t`로 바꾸고 public state로의
> 역변환을 제거한다.

둘 중 하나로 정리해야 한다. (참고: 내가 직접 확인한 바로는 `host_runtime_state_t`는
discovery 어휘가 아니라 **host 종료 절차의 진행 상태**이며,
`host_maintenance_runtime_t`가 `mark_serving()`/`terminate(intent)`와 함께 소유한다.
`retire`와 `shutdown`을 갈라 `retiring`/`draining`으로 전이한다. 6.9의 결론 방향 자체는
이 확인과 맞다.)

### 3.4 IC-02는 breaking change인데 확정이다 — **착수 전 정리 필요**

Node의 inline reentrancy를 금지하고 다음으로 바꾸는 것은 기존 사용 패턴을 깨뜨린다.

> yield 없이 현재 owner의 queued 결과를 기다리는 호출은 즉시 `InvalidOperation`으로
> 실패한다.

14장에 중단 조건이 있다.

> no-inline 전환이 공개 API의 합법적인 nested wait를 불가능하게 만듦

**중단 조건이 성립하면 `CONFIRMED` 결정을 되돌려야 한다.** 확정 시점이 이르다. WP1에서
실제 nested wait 사용처를 조사한 뒤 확정하는 편이 안전하다. EV-05 각주가 "Node가
잘못되었다는 근거만으로 사용하지 않는다"고 신중하게 적어 둔 것과, IC-02가 이미
`CONFIRMED`인 것도 어긋난다.

### 3.5 완료 정의가 닫히기 어렵다 — **검토 권고**

15장의 조건을 12장 × 4 runtime × (production + test + package + E2E)에 적용하면 사실상
프레임워크 전체 재검증이다.

> `DIVERGED`와 `UNVERIFIED`가 남아 있지 않다.

16장이 승인 묶음을 쪼갠 것은 좋으나 완료 정의는 전부-또는-전무라, 중간에 멈추면
"미완료"로만 남는다. **장별·영역별 부분 완료를 인정하는 기준**이 있어야 실제로 닫힌다.

---

## 4. 조치 우선순위

| 순위 | 항목 | 유형 | 이유 |
|---|---|---|---|
| 1 | IC-01 수치 근거 보강 | 착수 전 | 되돌리기 비용이 크고 중단 조건에 걸릴 수 있음 |
| 2 | IC-02 확정 시점 재검토 | 착수 전 | breaking change이고 중단 조건과 긴장 |
| 3 | `UNVERIFIED` IC의 재확정 가능성 명시 | 표기 | 3장과 5장의 진술 긴장 해소 |
| 4 | `host_runtime_state_t` 조사/확정 표기 통일 | 표기 | 2.3과 6.9 불일치 |
| 5 | 부분 완료 기준 추가 | 검토 | 완료 정의 실행 가능성 |
| 6 | 6.1에 .NET owner turn 10ms 추가 | 사소 | 사실 누락 |

---

## 5. 종합

Codex 계획은 조사 방법·근거·범위가 모두 내 문서보다 낫고, 내 오류를 이미 흡수·수정했다.
**이 문서를 기준으로 진행하는 데 동의한다.**

다만 IC-01과 IC-02는 **되돌리기 비용이 큰 결정인데 근거가 조사보다 앞서 있다.** 이 둘은
착수 전에 정리하는 편이 좋다. 나머지 셋은 문서 내부 일관성 문제라 표기 정리로 해결된다.

내 문서를 폐기한 경험에서 남길 교훈은 하나다 — **개수나 이름만 보고 판정하지 않는다.
그 값이 무엇을 표현하는지, 어디서 소비되는지까지 따라가야 한다.** Codex 문서의 EV 방식과
4단계 판정은 이 원칙을 구조로 강제하므로, 그 원칙이 WP1 실행 중에도 유지되는지가 이
계획의 성패를 가른다.
