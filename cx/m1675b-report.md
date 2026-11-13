# m1675b — Node payload 수명 정책 통합

결과: R1을 view 유지 정책으로 반영했습니다. 1MiB·4MiB reply를 각각 3,000건 처리한 공개 binding API 측정에서 view 방식의 지속적인 메모리 누적을 확인하지 못했습니다. Framework payload 유지 정책은 기존 `ZLinkBufferMessage.retainPayload` 한 곳으로 모았으며 completion과 binary reply의 불필요한 payload 복사를 제거했습니다. main 대비 새 실패는 0건입니다.

작업 위치: `D:/worktree/zlink-1639-msg-node`, `framework-node/1639-per-message-cost`. 시작 HEAD는 `4753c9b08d5e9d839da2de0708bfeaa1c83cbcc3`, 비교 main은 `40a875cf47`입니다. 시작 시 사용자 변경은 없었습니다. binding·스펙 수정, push·PR·merge·브랜치 전환은 수행하지 않았습니다.

## 변경 파일

- `framework/languages/node/packages/framework/src/runtime/backend/runtime-message.ts:20`: payload view 유지 정책의 단일 소유자.
- `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts:523`: reply parts를 닫기 전에 단일 정책으로 Buffer를 유지합니다.
- `framework/languages/node/packages/framework/src/runtime/backend/mesh-completion-table.ts:246`: 기존 managed Message는 재사용하고 binding Message는 같은 payload view를 runtime 표현으로 감쌉니다. close 이후 data가 무효라는 잘못된 주석을 삭제했습니다.
- `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts:349`: binary decoder의 별도 type 판정과 payload 복사를 제거했습니다.
- `framework/languages/node/test/contract/node-ingress-payload-ownership.test.js`: binding·managed completion 및 공개 socket으로 수신한 frame의 close 경계를 검증합니다.
- `framework/languages/node/test/contract/node-path-budget.test.js`: Buffer를 지우는 임의 fake 대신 실제 binding Message로 binary reply의 저장소 유지와 추가 복사 부재를 검증합니다.
- `framework/languages/node/test/contract/completion-terminal-conformance.test.js`: payload 접근 실패에도 terminal winner가 정확히 한 번 소비된다는 검증을 유지하고 오류 이름을 실제 동작에 맞췄습니다.

## 큰 reply 메모리 측정

WSL, Node `22.23.2`, 배포 binding `@zlink-systems/zlink@1.19.0`입니다. `memory.cjs`는 공개 Context·ROUTER·DEALER·Received·completion Poller만 사용합니다. inproc request-serial로 크기별 3,000건을 실행하고 매 100건 RSS·external·heapUsed·arrayBuffers를 기록했습니다. view는 `data()`를 유지하고 Message를 close하며, copy는 `Buffer.from(data())` 이후 close합니다. 응답 길이와 마지막 byte를 close 이후 확인하고 다음 요청으로 넘어갑니다. 실행 중 강제 GC는 없으며 종료 후 비교용 GC만 별도로 기록했습니다. 한 번의 실행마다 같은 reply Buffer를 송신하고, 응답 Buffer를 목록에 누적 보관하지 않습니다.

| reply | 정책 | 최대 RSS MiB | 최대 external MiB | 최대 heapUsed MiB |
|---|---|---:|---:|---:|
| 1MiB | view | 102.72 | 34.59 | 5.73 |
| 1MiB | copy | 102.82 | 35.59 | 5.72 |
| 4MiB | view | 116.07 | 41.60 | 5.74 |
| 4MiB | copy | 123.41 | 41.60 | 5.74 |

view의 external 값은 100건 간격 관측에서 1MiB 6회·4MiB 9회 감소했습니다. 3,000건의 누적 reply 크기인 3,000MiB·12,000MiB에 비례하는 누적은 없었습니다. view의 최대 RSS가 copy보다 높지도 않았습니다. 따라서 브리프의 조건에 따라 view 유지로 통일했습니다. 이 결과는 해당 Node·binding 버전과 워크로드의 관측이며 모든 환경의 GC 동작을 보장하는 판정은 아닙니다.

`bindings/node/native/src/addon_message_values.h:122–139`는 external Buffer에 frame 참조를 유지하고 finalizer에서 해제합니다. `napi_adjust_external_memory` 명시 호출의 부재만으로 메모리 미계상을 판정하지 않았습니다. 실제 공개 API 측정의 external·arrayBuffers가 증가와 감소를 보였으므로, 이번 실행에서는 binding 미계상 결함을 재현하지 못했습니다. binding 결함 BLOCKERS는 없습니다. `bindings/node/src/zlink/contracts/messaging/message.ts:309–328`은 read-only native receive를 data 접근 시 managed Buffer로 옮기는 경우도 있으므로, 모든 data Buffer를 native view로 간주하지 않습니다. Framework는 binding이 반환한 Buffer의 수명을 유지하며 추가 복사를 하지 않습니다.

## 수정 전 실패와 검증

수정 전 새 기대를 적용한 focused 검사에서 completion은 원본 view 변경 이후 예상 78·실제 110, binary reply는 예상 9·실제 8로 실패했습니다(`logs/r1-before-test.log`). 공개 socket으로 수신한 frame을 사용한 추가 검증도 예상 9·실제 7로 실패했습니다(`logs/r1-native-before.log`). 수정 후 동일 검증을 포함한 관련 4개 파일 22/22건이 통과했습니다(`logs/after-focused-test.log`). 잘못된 복사를 실제로 검출하는 수정 전 실패와 수정 후 통과를 모두 보존했습니다.

| 검사 | main | final |
|---|---:|---:|
| `npm test` runtime unit·contract aggregate | 2,618/2,618 | 2,657/2,657 |
| 같은 `npm test`의 m6a | 44/44 | 44/44 |
| `npm run verify:m6b-runtime` | 132/132 | 132/132 |
| `npm run verify:m6c-runtime` | 250/250 | 250/250 |
| 기본 전체 build·typecheck·runtime lint | exit 0 | exit 0 |
| `npm run lint` — samples 소스 포함 | exit 0 | exit 0 |
| `scripts/format/format.sh --check node` | exit 0 | exit 0 |

`npm test`는 저장소가 정의한 runtime label이며 browser E2E와 sample 실행 label은 포함하지 않습니다. #1688 이후 기본 build의 browser IIFE·Unity 동기화와 `unity-webgl-package.test.js`도 양쪽에서 통과했습니다. 저장소 Node root에서 `npm ci`를 한 번 수행하고 main 사본에 같은 의존성을 복사했습니다. sample별 npm install은 없으며 소스 검사는 repository 모드로 수행했습니다. benchmark Node workspace 의존성 준비는 별도의 benchmark root에서 수행했습니다. HTTP client는 동일 소스를 빌드해 패키지로 준비했으며 binding source 선택 없이 배포 1.19.0을 사용했습니다. 모든 테스트·ctest·샘플 실행은 WSL에서만 수행했습니다. `git diff --check`도 통과했습니다.

남은 실패: 0건. main 대비 새 실패: 0건. 스펙 gap BLOCKERS: 없음.

## 1024B 메시지당 계수

두 public NestJS host와 기존 framework benchmark transport·protobuf serializer를 같은 프로세스에서 실행했습니다. 기존 계측기의 연결 준비 1초와 초기 request 16건 이후 pattern별 warmup 2초·active 5초를 적용했습니다. warmup 완료분을 모두 수신한 뒤 계수를 초기화하고 active 뒤 미수신분을 drain하여 모든 유효 실행에서 count=received를 확인했습니다. 모든 계측 실행은 `flock /tmp/zlink-1639-measure.lock`으로 직렬화했습니다. CPU는 source·target 합계 user+system CPU이며 active와 마지막 drain을 포함합니다.

CPU·Promise·heap sampling은 각각 별도 프로세스에서 실행했습니다. Promise는 async_hooks의 PROMISE init 수입니다. 할당 byte는 HeapProfiler sampling interval 32KiB, 수거된 minor·major GC 객체 포함의 전체 JS 할당 추정치입니다. native malloc은 포함하지 않으며 Buffer factory의 반환 길이를 실제 할당으로 계산하지 않았습니다. throughput은 CPU 실행의 참고값이고 active 뒤 drain 시간을 포함합니다.

| pattern | 지표/메시지 | 수정 전 | 수정 후 |
|---|---|---:|---:|
| send | CPU µs | 31.295 | 38.031 |
| send | Promise 수 | 12.271 | 12.259 |
| send | JS sampling 할당 byte | 17,238.271 | 17,093.853 |
| send | 참고 throughput msg/s | 40,528.932 | 34,619.848 |
| request-serial | CPU µs | 362.222 | 289.752 |
| request-serial | Promise 수 | 45.347 | 45.519 |
| request-serial | JS sampling 할당 byte | 52,720.453 | 52,636.453 |
| request-serial | 참고 throughput msg/s | 2,600.853 | 3,655.626 |

각 지표는 전후 각각 한 번의 유효 실행입니다. CPU와 throughput의 인과적 개선·회귀는 이 결과만으로 판정하지 않습니다. 정상 benchmark 경로의 managed completion은 전후 모두 기존 part를 재사용하므로 R1에서 제거한 generic/native 경계 복사가 모든 request에 발생했다고 해석하지 않습니다. send의 정책 경로는 이번 수정 대상이 아닙니다. 정적 비용 근거는 generic completion과 binary reply의 전체 Buffer 복사 각각 한 곳 제거이며, 새 payload·closure·Promise·trace 문자열·native 호출은 추가하지 않았습니다.

## 결정 소유와 점검

| 결정 | 수정 전 판정 위치 | 단일 소유자·근거 |
|---|---|---|
| binding data Buffer의 close 경계 유지 | Node raw binding port:522, completion table:246, channel envelope:350 | Node runtime message:20, payload ownership §1·§2·§4 |
| 원래 binding Message close | raw binding port의 기존 finally, completion record의 기존 소유자 | 기존 close 경로 유지; Buffer 유지와 handle 해제를 혼동하지 않음 |
| completion terminal winner·실행 turn | 기존 completion table·execution gate | 변경 없음; 새 동기화나 terminal 상태 없음 |

소유 계층: Framework의 runtime message 소유자가 binding이 반환한 Buffer의 수명 경계 유지를 소유하고, binding은 data Buffer의 저장소 표현·native 참조 해제를 소유합니다.

Spec 조항: `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:21–29,33–41,59–75,99`의 binding 제약 구분·추가 복사 제거·경계 소유 이전·GC mapping이며 execution gate `02-handler-turn-and-execution-gate.ko.md` §2·§4의 기존 실행 경계를 유지합니다.

교차언어 대조: C++ `framework/src/runtime/backend/raw_binding_adapter.hpp:140–152`는 binding-facing owner에서 Framework 표현을 확보합니다. Java binding `Message.java:585,604–610`는 byte 배열을 복사해 반환합니다. .NET binding `Message.cs:150–157`는 native-backed ReadOnlyMemory를 managed 메모리로 옮기며 Framework `ZLinkMeshCompletionTable.cs:368–372`는 확보한 parts를 유지합니다. Node는 binding `message.ts:309–328`의 managed Buffer 또는 참조를 유지하는 Buffer를 기존 runtime 표현으로 보관합니다. 언어별 binding 표현 차이를 인정하는 같은 §1·§4 계약을 따르며 공개 reply 동작·해제 책임의 언어 차이나 새 계약은 추가하지 않았습니다.

변경 분류: B — binding Buffer 수명에 대한 잘못된 Framework 판정과 불필요한 복사 제거입니다. A/C/D 변경은 없습니다.

수정 전/후 판정 위치 수: payload 유지 정책 3→1. runtime representation을 감싸는 completion의 기존 instanceof 판정은 표현 재사용을 위한 것으로 유지했습니다. 상태·플래그·queue·lock·timer·retry 추가/삭제 0/0건. binary decoder의 수명용 instanceof 분기 1개와 잘못된 native 수명 주석을 삭제했습니다.

커밋 SHA: `70e79d1ca95a3222e8ad82421ac781dc4c14dd7c` (`Refs #1675`, `Refs #1639`). 지정 브랜치의 작업 트리는 clean입니다. 보고서·실행 스크립트·원수치·로그·SHA256 목록은 `D:/t/jobs/fx/cx/m1675b/`에 보존했습니다. 검증한 수정 파일 7개의 SHA256이 Windows 커밋 소스와 일치하고 진행 중인 검증 프로세스가 0임을 확인한 뒤 지정 WSL 사본을 제거했습니다. Project 상태는 Review로 갱신했습니다.

리팩토링 점검: 성능 2건·POSDDD 1건·불필요 코드 1건 발견, completion·binary reply의 payload 복사 2곳 제거·payload 수명 판정 단일 소유자 통합·잘못된 수명 주석과 binary instanceof 분기 삭제 완료, 수정 경로 잔여 0건·넘긴 항목 없음.

