# MP-6 보고서 — D-BP15 WRITABLE 재제출 회귀 테스트

## 결과

요청한 공개 C API 회귀 테스트 두 개를 Core integration suite에 등록했다. SEND는 inproc·tcp 모두
통과했지만 REQUEST는 두 transport 모두 현재 MP-3+4+5 patch에서 실패한다. 실패 지점은 B의
재제출이나 wire record 원자성이 아니라, router가 B request를 온전하게 받고 reply까지 성공한 뒤에도
A의 다른 request sequence가 열린 동안 B의 REQUEST completion이 dealer에 도착하지 않는 지점이다.

지시대로 `core/src`는 수정하지 않았다. 결과는 **테스트 추가 완료, REQUEST 회귀 RED**다.

## 시나리오

두 테스트는 같은 parameterized fixture를 사용하고 operation family만 REQUEST/SEND로 바꾼다.

1. DEALER→ROUTER를 inproc 또는 tcp로 연결하고 CONNECTION_READY monitor event를 기다린다.
2. application thread A가 2-part sequence의 `MORE`를 성공시킨 뒤 condition-variable barrier에서
   `FINAL` 전 상태로 대기한다.
3. completion owner 역할의 test thread B가 single-part filler를 `DONTWAIT`로 계속 제출한다. tcp의
   일시적인 local queue 거절 token은 공개 poller로 WRITABLE을 받아 닫고 계속 채운다. Receiver가
   진행하지 않은 상태에서 token이 pending인 실제 HWM/backpressure를 확인한 뒤에만 다음 단계로 간다.
4. B가 별도의 2-part record를 `DONTWAIT MORE`→`DONTWAIT FINAL`로 제출한다. FINAL의
   `BACKPRESSURED/EAGAIN`, nonzero token과 part 소비를 확인한다. 실패한 prefix는 Core에 남아 있지
   않으므로 이후 재제출은 첫 part부터 수행한다.
5. Receiver thread가 accepted filler를 모두 온전한 single-part record로 drain한다. B는
   `ZLINK_POLLCOMPLETION` poller에서 filler와 B의 WRITABLE을 ID/context로 구분해 모두 닫는다.
6. B가 같은 2-part payload를 처음부터 재제출한다. A는 여전히 barrier에서 열린 상태다.
7. Receiver가 B record를 정확한 2-part로 받고, REQUEST이면 reply한다. REQUEST completion을 B가
   받은 뒤에만 A의 barrier를 해제한다.
8. A가 자기 `FINAL`로 sequence를 닫고, receiver가 A record를 정확한 2-part로 받는다. REQUEST이면
   reply와 A completion까지 확인한다.

sleep 기반 동기화는 사용하지 않았다. Thread 순서는 condition variable, CONNECTION_READY monitor,
completion poller/token과 blocking public receive의 timeout으로만 제한한다.

## Assertion과 결과

| 단계 | assertion | REQUEST inproc | REQUEST tcp | SEND inproc | SEND tcp |
|---|---|---:|---:|---:|---:|
| A sequence open | A `MORE == OK`, ID 0, part consumed, FINAL barrier 유지 | PASS | PASS | PASS | PASS |
| 실제 포화 | filler FINAL이 `BACKPRESSURED/EAGAIN`, nonzero token; no-drain quiet 확인 | PASS | PASS | PASS | PASS |
| B 최초 제출 | `MORE == OK`; FINAL `BACKPRESSURED/EAGAIN`; nonzero token; prefix/final 소비 | PASS | PASS | PASS | PASS |
| WRITABLE | filler/B token을 ID·context로 식별; `WRITABLE/ADMITTED`, terminal errno 0 | PASS | PASS | PASS | PASS |
| B 전체 재제출 | 첫 part부터 제출; MORE/FINAL 성공; REQUEST ID nonzero, SEND ID 0 | PASS | PASS | PASS | PASS |
| B wire 원자성 | 정확히 2-part, `B-first/B-final`, 혼합·중복 없음 | PASS | PASS | PASS | PASS |
| B reply/completion | router reply 성공 뒤 B ID/context의 `REQUEST_OK` completion과 reply 1-part | **FAIL** | **FAIL** | 해당 없음 | 해당 없음 |
| A FINAL·wire 원자성 | B 완료 뒤 A FINAL 성공, 정확히 `A-first/A-final` 2-part | 앞 단계 실패로 미도달 | 앞 단계 실패로 미도달 | PASS | PASS |

REQUEST 실패의 최종 출력은 두 transport에서 같다.

```text
B REQUEST completion recv=201 errno=11 kind=1 id=0 ... request_result=0 reply_parts=0
```

`recv=201`은 `ZLINK_RECV_NO_DATA`, errno 11은 `EAGAIN`이다. Expected ID는 실행마다 달라지지만
nonzero이며, B 재제출 FINAL이 반환한 ID다. Receiver의 B 2-part 검증과 reply 성공 뒤에 이 실패가
발생하므로 payload 혼합이나 reply 제출 실패는 아니다.

## 변경 파일

| 파일 | 내용 |
|---|---|
| `core/tests/integration/test_writable_resubmit_from_other_thread_while_sequence_open.cpp` | REQUEST/SEND × inproc/tcp 공개 API fixture와 두 Unity case 추가 |
| `core/tests/CMakeLists.txt` | 위 executable을 split-only target으로 빌드하고 요청된 두 이름으로 CTest 등록; integration serial/network resource lock 적용 |
| `doc/plan/c016-worklog/progress-MP-6.md` | 진행·실패 기록 |
| `doc/plan/c016-worklog/core-rf-MP-6-report.md` | 이 보고서 |

등록된 CTest 이름은 다음 두 개다.

- `test_writable_resubmit_from_other_thread_while_sequence_open`
- `test_writable_resubmit_from_other_thread_while_sequence_open_send`

## 설계 비교

| 대안 | 장점 | 단점 |
|---|---|---|
| 기존 `test_phase3_request_reply_contract.cpp`와 `test_public_inproc_multipart_send.cpp`에 각각 추가 | 기존 helper 일부를 파일 안에서 직접 호출 가능 | transport와 operation별 흐름이 두 대형 executable에 갈리고, 정확한 두 회귀만 sanitizer·until-fail로 선택하기 어렵다 |
| 독립 split executable에서 fixture 하나를 REQUEST/SEND·inproc/tcp로 parameterize | 요청 이름으로 직접 선택 가능하고 네 셀이 같은 barrier/token 규칙을 공유한다 | 작은 fixture helper를 새 파일 안에 둬야 한다 |

두 번째 안을 선택했다. 결과 규칙은 “A가 연 sequence는 A만 닫고, B의 거절된 record는 exact
WRITABLE 뒤 첫 part부터 독립 재제출한다” 하나이며, 네 개의 복제 fixture 대신 하나의 흐름으로
유지했다. Runtime 규칙 수는 수정 전/후 모두 0개 추가다.

## 검증

모든 빌드와 테스트는 foreground로 실행했고 빌드 시작 전 `ninja`는 0개였다. 동시 ninja는 최대
1개, build parallelism은 4였다.

| 검증 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | PASS. 첫 compile에서 새 test enum 기본값 3건을 고친 뒤 최종 build 성공 |
| 새 두 CTest `--repeat until-fail:10` | REQUEST는 첫 회에 위 실패; SEND는 **10/10 PASS** |
| 관련 정규식 `part\|multipart\|request\|reply\|dealer\|router` 1회 | **45/45 PASS**, 88.40초 |
| ASan+LSan 새 두 CTest 1회 | REQUEST 기능 실패, SEND PASS. sanitizer/leak 진단 없음 |
| GCC TSan 새 두 CTest 1회 | 최초 일반 실행은 WSL ASLR `unexpected memory mapping`; MP-3 방식인 `setarch x86_64 -R`와 기존 `/tmp/mp2-tsan.supp`로 재실행해 REQUEST 기능 실패, SEND PASS. TSan race 진단 없음 |
| `git diff --check` 및 새 파일 whitespace check | PASS (`clang-format`은 host에 없음) |

## Source 보호 확인

요청한 `git diff --stat -- core/src core/include core/src/libzlink.vers`는 이 worktree에 이미 존재하던
MP-3+4+5 미커밋 patch 때문에 15개 `core/src` 파일을 출력한다(984 insertions, 468 deletions).
따라서 HEAD 기준 출력 자체는 비어 있지 않다.

MP-6 시작 전에 보존한 `mp5-before-mp6.patch`에서 source/include 구간만 계산한 SHA-256과 현재
같은 제한 경로 diff의 SHA-256은 모두 아래 값으로 동일하다.

```text
74831e8c788b25f75d1b770453eac17272aefeeaaafa6d98227626f770538197
```

즉 **MP-6가 추가한 `core/src`, `core/include`, `core/src/libzlink.vers` delta는 0**이다. 스펙도
수정하지 않았다.

## 계약 재확인과 분류

- 소유 계층: multipart sequence 격리, physical admission, WRITABLE과 REQUEST completion은 Core socket
  계층 소유다. 테스트는 public `zlink_send_part`, `zlink_request_part`, `zlink_router_recv_part`,
  `zlink_reply_part`, completion/poller/monitor API만 사용한다.
- 스펙: 현재 main 문서의 `core/doc/spec/core/socket/README.ko.md:950-965`는 thread별 sequence,
  다른 thread sequence의 독립성과 열린 sequence의 비인계를 규정한다. `:991-1020`은 DONTWAIT
  backpressure/token/WRITABLE을, `:1090` 이후 REQUEST admission/completion을 규정한다. 어느 문장도
  이 작업으로 다른 동작이 되지 않았다.
- 교차언어 대조: runtime 변경이 없어 구현 대조 대상은 아니다. 재현 형태는 D-BP15의 C++ binding
  completion owner 경로를 Core 공개 C API thread 두 개로 투영했다.
- 변경 분류: **B 기존 결함을 드러내는 회귀 테스트 추가**. SEND는 green이고 REQUEST completion
  progress만 red다.

## 성능과 멈춘 지점

테스트 전용 작업이라 성능 측정 대상이 아니다. REQUEST completion이 열린 A sequence 동안
`NO_DATA/EAGAIN`으로 남는 지점에서 멈췄다. 사용자 지시 때문에 원인 source를 수정하지 않았다.
patch는 미커밋 상태다.
