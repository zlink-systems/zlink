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

## 3. 이름 결정 (검토 필요)

- 네이티브 C 이름은 `copy`(공유)·`move`(이전)지만, 바인딩의 **깊은 복사**가 이미 `copy()`/`copyOf`/`from`을 쓰므로 `copy`를 그대로
  쓰면 "깊은 복사"와 혼동된다(현재 혼란의 원인). 따라서 **공유는 깊은 복사와 구별되는 이름**을 권장한다.
- 권장(감독 제안, 확정은 사용자):
  - ref-count 공유(`zlink_msg_copy`) → **`share()`**(또는 `shared()`): "같은 버퍼를 참조하는 새 핸들을 만든다"는 의미.
  - 이동(`zlink_msg_move`) → **`moveInto(target)`**/**`take()`**: 소유권 이전, 원본은 비워짐.
- 전 언어 **동일 이름·동일 시맨틱**으로 맞춘다. Java `sharedCopyOf`는 `share()`로 정렬하되 기존 이름은 한 사이클 deprecate alias로 남긴다(호환).

## 4. 언어별 변경 범위

- **C++**: `message_t`에 `share()`(→ `zlink_msg_copy`) 공개 멤버 추가. move는 `zlink_msg_move`를 감싸는 명시적 API로 노출(C++ move 시맨틱에만 의존하지 않음 — 다른 바인딩과 동일 계약). 내부에서 이미 `zlink_msg_copy`를 쓰므로 파괴적 변경 아님.
- **.NET**: `Message`에 `Share()`(→ `zlink_msg_copy`, 이미 `Message.Native.cs`에 P/Invoke 존재) 공개 멤버 추가. 이동 멤버도 추가.
- **Node**: `Message`에 `share()`(→ addon `zlink_msg_copy`) 공개 멤버 추가. 이동 멤버도 추가.
- **Java**: `sharedCopyOf`/`moveInto`/`takeParts`를 통일 이름으로 정렬(behavior 불변, alias로 호환).
- **C**: 레퍼런스(변경 없음). perf relay는 이미 move/copy 사용.

모두 **public interface 추가/이름 정렬**이며 ownership·error 계약은 유지한다. .NET·Node·C++ 추가는 non-breaking, Java 이름 정렬만 deprecation 주의.

## 5. perf harness 정렬 (동일 측정 의미)

- 각 언어 routed echo relay(SENDSEND server)와, 해당되면 reqrep server reply가 **C와 동일하게 받은 메시지를 `move`로 send에 넘기고**
  (relay는 안 들고 있으므로), 재제출이 필요하면 `share`로 snapshot을 든다. **깊은 복사 제거.**
- 바꾸지 않는 것: 실패 처리·retry 로직·metric·sampler·timeout·HWM·client 수·duration·측정 흐름. (수치 조작 금지, §7.0.1 parity)
- 공개 API로 zero-copy echo가 되면 관련 harness는 그 멤버를 쓴다.

## 6. 반영 순서

1. (이 문서 검토·승인)
2. 스펙 반영: `bindings/doc/spec/**`의 공통 수신 ownership 계약과 각 언어 README의 Message 절에 `share()`/이동 멤버와 그 수명 계약을 명시.
   (보호 경로이므로 감독이 직접, 사용자 승인 범위에서.)
3. 바인딩 코드: 언어별 `Message` 멤버 추가/정렬 + 단위·계약 테스트.
4. 교차언어 parity 테스트: 같은 메시지를 share/move했을 때 refcount·payload·수명이 언어별로 동일하게 관측되는지.
5. perf relay 정렬(deep-copy → move/share).
6. 재측정: SENDSEND(및 routed) before/after를 canonical C baseline과 paired로 기록. 비대상 회귀 없음 확인.

## 7. 계약·수명 의미 (스펙에 명시할 것)

- `share()`: 반환 메시지는 원본과 **같은 payload 버퍼를 공유**하고 refcount가 증가한다. 원본·사본 모두 유효하며 각자 `close`한다.
  payload는 immutable 관측(공유 중 변경 금지). Core `msg_t::copy` 시맨틱과 일치.
- `move`/`take`: 원본의 소유권을 대상으로 **이전**하고 원본은 empty가 된다. refcount 불변.
- 이 계약은 전 언어 동일해야 한다(교차언어 대조 필수).

## 8. 리스크

- Java 이름 정렬은 기존 `sharedCopyOf` 사용처에 영향 → deprecation alias로 완충.
- 공유 후 수명 오사용(공유 중 원본을 mutate하거나, 한쪽만 close 후 다른 쪽 사용) 방지를 테스트로 강제.
- perf relay가 move로 바뀌면 재제출/드레인 경로에서 소유권이 이미 이전됐음을 전제로 해야 하므로, 재제출이 필요한 언어는 C처럼 share snapshot을 병행.

## 9. 검증

- 단위·계약 테스트(share/move 수명·refcount), 교차언어 parity 테스트 통과.
- SENDSEND tcp 작은 size(64/256/1024) before→after 공정 비율 상승 확인, 큰 payload·PAIR 회귀 없음.
- 결과는 `doc/perf/perf/bindings-0.17.5/`의 상세 표·요약표에 반영.
