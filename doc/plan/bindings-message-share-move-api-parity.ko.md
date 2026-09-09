# bindings 메시지 공유(share)·이동(move) 공개 API 통일 및 perf relay parity 계획

> 작성일: 2026-09-09 · 작성: 머신 B(감독) · 상태: 계획(코드·스펙 반영 전)
>
> 발단: bindings 0.17.5 성능 캠페인에서 routed echo(SENDSEND)의 C 대비 비율이 작은 payload에서
> 유난히 낮은 원인을 프로파일한 결과, **C perf relay와 managed binding relay가 서로 다른 일을
> 하고 있어 "동일 측정 의미" 비교가 깨져 있음**을 확인했다. 이 문서는 그 비대칭을 없애기 위한
> 공개 API 통일과 perf 정렬 계획을 담는다. 코드·스펙은 이 계획 검토 뒤 반영한다.

## 1. 배경과 근거 (코드로 확인)

### 1.1 Core 네이티브 메시지는 refcount 공유 버퍼다
- `core/src/runtime/core/msg.hpp`: "Shared message buffer … refcount member … `shared=128` flag". atomic refcnt 보유.
- C API(`core/include/zlink/message/api.h`)에 두 프리미티브가 있다:
  - **`zlink_msg_copy(dest, src)`** = **ref-count 공유**. 구현(`core/src/runtime/core/msg.cpp:486` `msg_t::copy`):
    src가 이미 shared면 `refcnt->add(1)`, 아니면 shared 플래그를 세팅하고 `refcnt=2`로 만든다.
    → dest·src가 **같은 버퍼를 공유**하고 각자 독립적으로 `close`한다(깊은 복사 아님).
  - **`zlink_msg_move(dest, src)`** = **소유권 이전**. refcount를 올리지 않고 핸들만 옮기며 src를 비운다(가장 쌈).
  - 조회: `zlink_msg_refcnt(msg, err)`.

### 1.2 C perf relay(SENDSEND echo 서버)가 실제로 쓰는 것
`bindings/c/perf/multi/common/perf_multi_relay_server.hpp`:
- **echo 주 경로 = `zlink_msg_move`**(`:138`): 받은 part를 pending reply로 **이전**한다(relay는 안 들고 있으니 공유보다 이전이 쌈).
- **재제출 snapshot = `zlink_msg_copy`**(`:217`): 백프레셔로 재전송해야 할 때만 refcount 공유로 참조를 하나 더 든다.
즉 C echo = **move(주) + copy/share(재제출 대비)**, 둘 다 **복사 없음**.

### 1.3 바인딩 공개 API는 불일치·불균등하다
| 언어 | ref-count 공유(zlink_msg_copy) 공개 노출 | 이동(zlink_msg_move) 공개 노출 | 비고 |
|------|------------------------------------------|---------------------------------|------|
| C | `zlink_msg_copy`(직접) | `zlink_msg_move`(직접) | 레퍼런스 |
| Java | **있음** — `Message.sharedCopyOf`(내부 `MH_MSG_COPY`=zlink_msg_copy, `NativeMessage.java:28`) | 있음 — `moveInto`/`moveTo`/`Received.takeParts` | 유일하게 둘 다 공개 |
| C++ | 없음(공개 멤버) — 내부에서만 `zlink_msg_copy` 사용(`message.cpp:40,59`, `native_message_parts.hpp:271,347`) | 없음(공개) — `message_t` C++ **move 시맨틱**으로 대체, C API 미노출 | `ref_count()` 조회만 공개 |
| .NET | 없음(공개) — 내부 `zlink_msg_copy`(`Message.Native.cs:301`) | 없음(공개) | `RefCount` 조회 + `CopyTo`(깊은 복사)만 |
| Node | 없음(공개) — 내부 `zlink_msg_copy`(addon `addon_core.cc:1175`) | 없음(공개) | `_refCount` 내부 + `copy()` 깊은 복사만 |

→ **결과:** managed relay(예: Java `PerfMultiRoutedRelay.java`)는 routing-id 방어 복사 + payload **깊은 복사**로 echo하고,
.NET·Node는 공개 API로 zero-copy echo 자체가 불가하다. C relay는 move/copy로 복사 없이 echo한다. **동일 의미 비교가 아니다**
(SENDSEND 작은 payload 비율이 이 비대칭 때문에 실제보다 낮게 나온다; §근거: 프로파일에서 SENDSEND 64B는 양쪽 CPU 포화이고
Java relay의 deep-copy가 고정 per-message 비용의 큰 부분).

## 2. 목표

1. **전 바인딩이 동일한 네이티브 프리미티브(`zlink_msg_copy`·`zlink_msg_move`)를 감싸는 일관된 이름의 `Message` 멤버 함수를 공개**한다.
   C++·.NET·Node에 추가하고, Java는 기존 것을 통일 이름으로 정렬한다. C++은 C++ move 시맨틱 대신 **C API 함수를 직접** 감싼다.
2. **perf routed echo(SENDSEND) relay를 전 언어가 C와 동일하게 `move`(+필요 시 재제출 `share`)로 echo**하도록 바꿔 **동일 측정 의미**로 만든다.
   (수치를 올리려는 게 아니라 C와 같은 연산을 하도록 맞추는 parity 교정이다.)
3. 위를 **스펙 문서에 반영**하고 **교차언어 parity 테스트**를 추가한 뒤, SENDSEND(및 routed)를 재측정해 공정 비율을 기록한다.

## 3. 이름·시그니처 명세 (언어별)

**C API 이름과 1:1로 통일한다** — `zlink_msg_copy → Copy`, `zlink_msg_move → Move`. 바인딩만의 별도 이름(`share`/`moveInto` 등)을
만들지 않는다. "이 언어의 `Copy`는 곧 C의 `zlink_msg_copy`(=ref-count 공유)"가 자명해지고, 두 함수가 정확히 대칭이 된다.

### 3.1 통일 시맨틱
- **`Copy`** = `zlink_msg_copy`. **호출한 메시지의 네이티브 버퍼를 공유하는 새 메시지를 반환**한다(refcount++). 원본과 사본 모두
  유효하며 각자 close한다. 원본은 비워지지 않는다. payload는 공유 중 immutable. (Core `msg_t::copy`와 일치.)
- **`Move`** = `zlink_msg_move`. **호출한 메시지의 소유권을 대상으로 이전**한다. 대상의 이전 내용은 close되고, **호출한 메시지는
  empty**가 된다. refcount는 증가하지 않는다.

### 3.2 이름 충돌 해소 — 기존 "깊은 복사"는 `Clone`으로
기존 바인딩은 `copy()`/`copyOf`/`CopyTo`를 **깊은 복사**(독립 버퍼)에 쓴다. `Copy`를 C와 동일한 ref-share로 확정하므로, 그 기존
깊은 복사는 이름을 **`Clone`(독립 복제)** 으로 옮긴다. 그러면 세 동작이 이름만으로 구분된다:
- `Copy` = ref-count 공유(같은 버퍼) — C `zlink_msg_copy`.
- `Move` = 소유권 이전(원본 empty) — C `zlink_msg_move`.
- `Clone` = 깊은 복사(독립 버퍼) — **네 언어 모두 제공해 API를 통일한다.** 반환된 사본은 refcount를 공유하지 않고
  독립적으로 수정·close된다. **Node**는 기존 공개 `copy()`(깊은복사)를 `clone()`으로 이름 이동, **Java**는 독립 버퍼
  `clone()` 제공(테스트로 refcount=1·독립 mutation 검증). **C++**은 `message_t clone() const`, **.NET**은
  `Message Clone()`을 새로 추가(payload를 새 native frame에 깊은 복사). .NET 기존 `CopyTo(Span)`은 payload를 버퍼에
  채우는 span-fill이라 별개로 유지된다.

### 3.3 언어별 시그니처 (두 함수 각각)
언어별 관용 케이싱을 따르되 의미어(Copy/Move/Clone)는 동일하게 맞춘다.

| 언어 | `Copy` (= zlink_msg_copy, ref-share) | `Move` (= zlink_msg_move, 이전) | 기존 깊은복사 → `Clone` |
|------|--------------------------------------|--------------------------------|------------------------|
| **C** | `ZLINK_EXPORT zlink_config_result_t zlink_msg_copy(zlink_msg_t *dest, zlink_msg_t *src);` (기존) | `ZLINK_EXPORT zlink_config_result_t zlink_msg_move(zlink_msg_t *dest, zlink_msg_t *src);` (기존) | — (C엔 없음) |
| **C++** | `message_t message_t::copy() const;` — 새 `message_t` 반환(버퍼 공유). 내부 `zlink_msg_copy(new, this)`. `this` 유효 유지. | `void message_t::move(message_t &dest);` — `zlink_msg_move(dest, this)`, `this`는 empty. **C++ move 시맨틱이 아니라 C API를 직접** 감쌈. | `message_t message_t::clone() const;` — 독립 버퍼로 깊은 복사(신규 추가) |
| **.NET** | `public Message Copy();` — 새 `Message` 반환. 내부 `zlink_msg_copy(ref dest, ref this)`(P/Invoke 존재, `Message.Native.cs`). | `public void Move(Message dest);` — `zlink_msg_move`, `this`는 empty. | `public Message Clone();` — 독립 버퍼로 깊은 복사(신규 추가). 기존 `CopyTo(Span/IBufferWriter)`는 span-fill이라 별개로 유지. |
| **Node/TS** | `copy(): Message;` — 새 `Message` 반환. addon `zlink_msg_copy`(존재, `addon_core.cc`) 노출. | `move(dest: Message): void;` — addon `zlink_msg_move`, `this`는 empty. | 기존 `copy()`(깊은복사) → `clone(): Message`로 개명 |
| **Java** | `public Message copy();` — 새 `Message` 반환(내부 `MH_MSG_COPY`=`zlink_msg_copy`). | `public void move(Message dest);` — `zlink_msg_move`. | `public Message clone();` — 독립 버퍼 deep copy(`Message.from(Message)` 재사용). 기존 `sharedCopyOf`/`moveInto`/`moveTo`는 **비공개(bridge/package-private)**라 공개 alias 불필요. |

- **반환형 규칙:** `Copy`/`Clone`은 **새 메시지를 값으로 반환**(C의 out-param `dest`는 반환형으로 감쌈). `Move`는 대상을 인자로
  받고 **void**(호출 메시지를 비운다).
- **에러:** 네이티브 실패는 각 언어 표준(C `zlink_config_result_t`, C++/Java/.NET/Node 예외)으로 전파. 조용한 성공/무시 금지.
- **조회 API는 그대로:** `ref_count()`/`RefCount`/`_refCount`(내부)/`zlink_msg_refcnt` 유지(변경 없음).
- **호환:** 대부분의 추가는 non-breaking이다. 유일한 breaking은 **Node** — 기존 공개 `copy()`가 깊은복사였는데 통일 `copy`=ref-share로
  바뀌므로 동일 시그니처 alias가 불가능하다. major 버전 breaking change로 처리하고(§3.2), 마이그레이션(`copy`→`clone`)은 문서로 안내한다.

## 4. 언어별 변경 범위 (구현 결과 반영)

- **C++**: `message_t`에 `copy()`(→ `zlink_msg_copy`)·`move(dest)`(→ `zlink_msg_move`)·`clone()`(독립 버퍼 deep copy) 공개 멤버 추가. move는 C API를 직접 감싼다(C++ move 시맨틱에 의존하지 않음). 기존 공개 깊은복사 없음(개명 대상 없음). 완료.
- **.NET**: `Message`에 `Move(dest)`(→ `zlink_msg_move`)·`Clone()`(독립 버퍼 deep copy) 추가. `Copy()`(→ `zlink_msg_copy` ref-share)는 이미 존재. 기존 `CopyTo(Span/IBufferWriter)`는 span-fill이라 **유지**(Message deep copy 아님, 개명·deprecation 없음). 완료.
- **Node**: `copy()`(→ addon `zlink_msg_copy` ref-share)·`move(dest)`(→ addon `zlink_msg_move`) 추가, **기존 `copy()`(깊은복사)→`clone()`로 이동**. **breaking**(동일 시그니처 alias 불가) — 마이그레이션은 문서로 안내. (구현 진행 중.)
- **Java**: `copy()`(→ `zlink_msg_copy`)·`move(dest)`(→ `zlink_msg_move`)·`clone()`(deep copy) 공개 추가. 기존 `sharedCopyOf`/`moveInto`/`moveTo`는 **공개 API가 아니라 내부 bridge/package-private**였으므로 공개 deprecated alias는 두지 않고 내부 호출 경로만 유지. 완료.
- **C**: 레퍼런스(변경 없음). perf relay는 이미 move/copy 사용.

C++·.NET·Java의 추가는 non-breaking(이름 정렬 alias 불필요). **Node만 breaking**(§3.2).

## 5. perf harness 정렬 (동일 측정 의미)

- 각 언어 routed echo relay(SENDSEND server)와, 해당되면 reqrep server reply가 **C와 동일하게 받은 메시지를 `Move`로 send에 넘기고**
  (relay는 안 들고 있으므로), 재제출이 필요하면 `Copy`로 snapshot을 든다. **깊은 복사 제거.**
- 바꾸지 않는 것: 실패 처리·retry 로직·metric·sampler·timeout·HWM·client 수·duration·측정 흐름. (수치 조작 금지, §7.0.1 parity)
- 공개 API로 zero-copy echo가 되면 관련 harness는 그 멤버를 쓴다.

## 6. 문서 반영 계획 (spec·guide·README)

이 API는 코드뿐 아니라 **문서 3계층**에 반영해야 한다. 각 대상 파일과 넣을 내용을 명시한다.

### 6.1 스펙(`bindings/doc/spec/**`, 보호 경로 — 감독이 직접, 사용자 승인 범위)
- **공통 Message ownership 계약** 절에 세 동작을 정식 정의: `Copy`(ref-share)·`Move`(이전)·`Clone`(깊은복사) — 각 refcount·수명·
  payload immutability·empty 여부. 전 언어 동일 시맨틱임을 규범(normative)으로 기재.
- 언어별 spec 문서의 `Message` API 절에 §3.3 시그니처 표를 반영(관용 케이싱·반환형·에러).
- 기존 깊은복사 이름(`copy`/`CopyTo`) → `Clone` 개명과 **deprecated alias 유지 기간**을 명시.

**대상 파일(확정):**
| 파일 | 반영 지점 | 넣을 내용 |
|------|-----------|-----------|
| `bindings/doc/spec/draft/message-ownership.ko.md` | §"명시적 copy"(L109), §"언어별 표현 범위"(L348), §"Contract test 요구사항"(L444) | `Copy`/`Move`/`Clone` 3동작 규범 정의로 분리. **주의:** 현행 문서의 `copy()`는 이미 "독립 ownership·deep copy 미보장(=ref-share 허용)"으로 서술되어 우리 `Copy` 의미와 사실상 일치 → `Copy`로 명명 확정하고, 깊은복사는 `allocate`+`copy_to` 대신 `Clone`으로 정리. `moveMessage()` 관련 문단은 `Move` 명명과 정합. contract test 요구사항에 3동작 항목 추가. |
| `bindings/doc/spec/cpp/README.ko.md` / `.en.md` | Message/소유 절 | C++ `copy()`/`move(dest)`/`clone()` 시그니처·계약 |
| `bindings/doc/spec/dotnet/README.ko.md` / `.en.md` (+ `api-reference-comments.{ko,en}.md`) | Message API 절 | .NET `Copy()`/`Move(dest)`/`Clone` + `CopyTo` deprecation |
| `bindings/doc/spec/node/README.ko.md` / `.en.md` | Message API 절 | Node `copy()`/`move(dest)`/`clone()` + 기존 `copy`(깊은복사) 의미 전환 경고 |
| `bindings/doc/spec/java/README.ko.md` / `.en.md` | Message API 절 | Java `copy()`/`move(dest)`/`clone()` + `sharedCopyOf`/`moveInto` alias |

### 6.2 guide/사용법 문서 — **없으면 추가**
- 각 언어 가이드의 Message/소유권 절에 **사용 예제**를 추가: (a) `Copy`로 공유 후 양쪽 각자 close, (b) `Move`로 send에 넘기기
  (echo relay 패턴), (c) `Clone`으로 독립 복제가 필요한 경우. **"언제 어느 것을 쓰나"** 결정 가이드 한 단락 포함.
- 단순 시그니처 나열이 아니라 소유권·수명 관점 사용법으로 쓴다. 현재 가이드에 관련 서술이 없으므로 **신규 소절로 추가**.

**대상 파일(확정):**
| 파일 | 반영 지점 | 넣을 내용 |
|------|-----------|-----------|
| `bindings/doc/guide/cpp/index.ko.md` / `.en.md` | `### 메시지`(L87), `## 소유권과 수명`(L173) | `Copy`/`Move`/`Clone` 사용 예제 + 결정 가이드 |
| `bindings/doc/guide/dotnet/index.ko.md` / `.en.md` | `### 2. 메시지`(L94), `## 소유권과 수명`(L182) | 동일 |
| `bindings/doc/guide/node/index.ko.md` / `.en.md` | `### 메시지`(L86), `## 소유권과 수명`(L155) | 동일 |
| `bindings/doc/guide/java/index.ko.md` / `.en.md` | `### 2. 메시지`(L120), `## 소유권과 수명`(L243) | 동일 |
| `bindings/{dotnet,node}/README.md`, `bindings/cpp/README.doxygen.md`, `bindings/java/README.javadoc.md`, `bindings/node/README.typedoc.md`, `bindings/dotnet/README.docfx.md` | Message 절 | API 표면 doc의 Message 멤버 목록에 `Copy`/`Move`/`Clone` 반영 |

> 라인 번호는 조사 시점(현재 main) 기준 앵커이며 편집 전 재확인한다. en/ko는 동일 내용으로 동기화한다.

### 6.3 반영 순서
1. (이 문서 검토·승인)
2. **스펙 반영**(§6.1) — 감독이 직접, 사용자 승인 경로/범위 확인 후.
3. **guide/README 사용법 반영**(§6.2) — 없으면 추가.
4. 바인딩 코드: 언어별 `Message` 멤버 추가/정렬(`Copy`/`Move`, 깊은복사→`Clone`) + 단위·계약 테스트.
5. 교차언어 parity 테스트: 같은 메시지를 `Copy`/`Move`했을 때 refcount·payload·수명이 언어별 동일 관측.
6. perf relay 정렬(deep-copy → `Move`/`Copy`).
7. 재측정: SENDSEND(및 routed) before/after를 canonical C baseline과 paired로 기록. 비대상 회귀 없음 확인.

## 7. 계약·수명 의미 (스펙에 명시할 것)

- `Copy`: 반환 메시지는 원본과 **같은 payload 버퍼를 공유**하고 refcount가 증가한다. 원본·사본 모두 유효하며 각자 `close`한다.
  payload는 immutable 관측(공유 중 변경 금지). Core `msg_t::copy` 시맨틱과 일치.
- `Move`: 원본의 소유권을 대상으로 **이전**하고 원본은 empty가 된다. refcount 불변.
- `Clone`: **독립 버퍼**로 깊은 복사. 원본과 무관(refcount 공유 없음).
- 이 계약은 전 언어 동일해야 한다(교차언어 대조 필수).

## 8. 리스크

- 이름 정렬(`sharedCopyOf`·`moveInto`·`CopyTo`·Node `copy`)은 기존 사용처에 영향 → deprecation alias로 완충.
- **`Copy` 의미 전환 주의**: Node/.NET에서 `copy`/`CopyTo`가 깊은복사였다가 `Copy`가 ref-share가 되므로, 개명 없이 의미만 바뀌면
  조용한 오동작 위험 → 반드시 기존 깊은복사를 `Clone`으로 개명하고 alias는 이전 의미(깊은복사)를 가리키게 한다.
- 공유 후 수명 오사용(공유 중 원본 mutate, 한쪽만 close 후 다른 쪽 사용) 방지를 테스트로 강제.
- perf relay가 `Move`로 바뀌면 재제출/드레인 경로에서 소유권이 이미 이전됐음을 전제로 해야 하므로, 재제출이 필요한 언어는 C처럼 `Copy` snapshot을 병행.

## 9. 검증

- 단위·계약 테스트(`Copy`/`Move`/`Clone` 수명·refcount), 교차언어 parity 테스트 통과.
- SENDSEND tcp 작은 size(64/256/1024) before→after 공정 비율 상승 확인, 큰 payload·PAIR 회귀 없음.
- 결과는 `doc/perf/perf/bindings-0.17.5/`의 상세 표·요약표에 반영.
