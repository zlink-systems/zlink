# Node L2 header 복사와 잔여 비용

Request/reply header의 중간 JS Buffer와 binding 최초 제출 복사를 제거했다. 최종 native storage에 template를 쓰는 1회 복사와 수신 복사는 남는다. **CPU·heap 표본은 증가했다. 처리량 개선이나 7% 예산 달성을 주장하지 않는다.**

아래 N은 `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts`, D는 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs`다. Binding 상대 경로는 `bindings/node/`다. 전은 `51d327d236`의 codec이며, 후 line은 최종 source 기준이다.

| 메시지당 비용, flow Off | Node 전 / 근거 | .NET reference / 근거 |
|---|---|---|
| outbound request 272B / reply 213B | 중간 managed header Buffer 1개, 전체 template 복사 1회 + binding native storage 복사 1회. N 전:646,720–725; `native/src/addon_core.cc:211–222,904–924` | native Message 1개에 prefix 복사 + dynamic field 직접 기록. 중간 byte storage 0. D:1195–1236; `Runtime/Channels/ZLinkChannelReplyWriter.cs:41–60` |
| inbound request / reply | header Buffer 1개, native→JS 전체 복사 1회. `src/zlink/contracts/messaging/message.ts:309–326`, `runtime/messaging/message_materializer.ts:92–98`, `native/src/addon_message_values.h:248–265`, `addon_core.cc:509–525,2138–2150` | native span 직접 읽기, 추가 header 복사 0. D:377–378 |
| header decode string | request 5 / reply 4 / command 3. N 후:1077–1078. 생성 request는 correlation 1 + ISO 1, N:129,537 | request 4 / 실제 L2 reply 3. D:486–506,578–582. Deadline은 value type이며 실제 reply messageName은 빈 문자열 |
| correlation Map | wire correlation string 키 0. Binding native token은 source당 set 2 + get 1 + has 1 + delete 3 = **7회**, L1/L2 동일. `runtime/messaging/completion_owner.ts:625–640,769–775` | L2 wire correlation 문자열 table 없음; native completion 상관관계는 L1에도 존재 |

String 계수는 escape 없는 실제 셀 기준이다. Flow를 읽으면 +1, metadata entry마다 key/value +2이며 escape decode의 JSON source 문자열은 별도다. Prepared template cache는 encode마다 기존 WeakMap get 1 + Map get 5(N:696–707)이며 correlation table과 구분한다.

## 변경과 소유

- N:651–680에서 기존 `ServiceWireOwnedFrame.allocate`→`data`→`takeMessage`로 request/reply와 일반 header를 최종 native storage에 쓴다. Generic JSON writer는 하나이며 prepared template도 같은 writer의 native view에서 만든다(N:709–727). 기존 flow Off command의 shared template 재사용은 유지한다.
- 기존 `submitNativeParts`를 raw port에서 `backend/runtime-message.ts:80–100`으로 옮겼다. Raw와 ClientServer request/reply가 같은 동기 실패 해제 규칙을 사용한다. `closeMessages`의 기존 중복 구현도 이 파일로 모았다.
- Mesh multipart는 header view를 읽은 뒤 원래 native handle을 해제한다(`backend/node/node-raw-mesh-backend.ts:2978–2985`). Stateful 로컬 변환도 같은 release를 사용한다(`foundation/service-stateful-runtime.ts:4825–4833`). Header를 native Message로 반환하면서 필요한 수명 처리이며 새 상태는 없다.
- 변경 파일 전체는 [raw/changed-files.txt](raw/changed-files.txt), production source/dist SHA256과 freshness는 [raw/final-code-hashes.json](raw/final-code-hashes.json)에 있다. Production 9개와 contract test 12개다. Binding·스펙 변경은 없다.

소유 계층: Framework channel codec이 bytes를 정하고 기존 native frame owner와 binding이 저장소·제출 수명을 담당한다.

스펙: ClientServer §5.3, messaging hot path §3 E1, payload ownership §1–5, binding message ownership의 allocate/submit 계약.

교차언어: .NET D:1195, C++ `envelope_codec.cpp:477–486`, Java/Kotlin 공유 `ZLinkChannelEnvelope.java:497,568–577`을 대조했다. Node 내부 저장소만 변경하며 wire 값·필드 순서·다른 언어의 동작은 바꾸지 않는다.

변경 분류: **B — 기존 내부 복사 비용 수정**. 대안인 두 번째 owner/별도 Buffer writer는 사용하지 않았다. 구현 전 소유 표는 [raw/decision-ownership-before.md](raw/decision-ownership-before.md)다.

| 변경 범위의 규칙 정의·판정 위치 | 전 | 후 | 위치 |
|---|---:|---:|---|
| header bytes: JSON writer + prepared dynamic patch | 2 | 2 | 전 N:728,722–725 → 후 N:740,672–675 |
| header 저장소 생성: generic JS / prepared JS / binding byte 입력 | 3 | 1 | 전 N:646,720 + addon_core.cc:211 → 후 N:651의 final native frame |
| 동기 제출 실패 해제 | 1 | 1 | 기존 raw port catch:537 → 공용 submitNativeParts:97 |
| 해제 대상 판정 | 4 | 2 | 전 channel closeMessages:471, raw port:538·540, stateful:4834 → 후 runtime-message:66, raw-mesh:2984 |
| header native write 실패 해제 | 0 | 1 | 후 N:678–680, 기존 frame.close 재사용 |
| **합계** | **10** | **7** | 단순 API 위임과 변경 없는 binding 내부 판정은 중복 계수하지 않음 |

새 flag/queue/lock/cache/timer 0. 기존 cache만 유지했다. 리팩토링 점검은 JS 중간 저장소와 submit/release 중복을 정리했으며 execution gate 중복 동기화 발견 0, 별도 이슈로 넘긴 항목 0이다.

## 전후 계수와 프로파일

| 메시지당, request / reply | 전 | 후 |
|---|---:|---:|
| managed header Buffer / native header Buffer view | 1 / 0 | 0 / 1 |
| template→중간 JS header 복사 | 1회, 272 / 213B | **0** |
| template→최종 native header 기록 | 0 | 1회, 272 / 213B |
| 최초 binding submit의 header payload 복사 | 1회, 272 / 213B | **0** |
| inbound native→JS header 복사 | 1회, 272 / 213B | 1회, 272 / 213B |
| decode string: target request / source reply | 5 / 4 | 5 / 4 |
| wire correlation Map / binding token Map | 0 / 7회 | 0 / 7회 |

한 왕복의 전체 header byte 복사는 **6회/1455B → 4회/970B**다. Native frame 구조체·reference-count 조작과 Core TCP/framing·body protobuf 복사는 제외했다. Source completion의 복사는 L1에도 있으므로 이 전체를 L2 추가 비용으로 보지 않는다. Header Buffer 객체 자체가 없어진 것은 아니며 native storage의 view로 바뀌었다. 기존 owner wrapper도 메시지마다 생성한다.

직접 caller만 세는 계수기에서 전 source/target 각3060개에 allocUnsafe/copy 각3060회, 후 각5540개에 managed alloc/copy 0회·native allocate 5541회(초기 template 1회 포함)·native set 5540회를 확인했다. Decode string은 전 target15300/source12240, 후 target27700/source22160으로 정확히 5/4개다. Source bigint Map도 요청5540개에 set11080/get5540/has5540/delete16620회다. 근거: `raw/node/audit-direct-before-L2/`, `audit-final-L2/`, `audit-maps-L1/`.

이 계수는 **최초 submit OK 경로**다. 계측 요청은 모두 OK였으며 실제 native BACKPRESSURED는 0회다. Binding의 EAGAIN snapshot은 `completion_owner.ts:75–84,441`의 Buffer 복사와 이후 재제출 복사를 추가할 수 있다. 이 binding 소유 동작은 변경하지 않았다.

Ladder `c019495a82`를 git archive로 `n2hdr-scratch`에 추출하고 own packages에 연결했다. Node source/target 및 모든 thread는 CPU `6–7,14–15`, warmup1s + active5s, CPU sampling500µs + allocation sampling32KiB, hard timeout19s+kill-after1s였다. Measure lock은 사용하지 않았다. Baseline codec만 같은 compiler/config로 own dist에 임시 빌드한 뒤 최종 module을 복원했다([raw/baseline-module-provenance.json](raw/baseline-module-provenance.json)).

| 유효 최종 CPU profile | active 제출=완료=수신 | errors |
|---|---:|---:|
| own-before-L1 | 103688 | 0 |
| own-before-L2 | 67072 | 0 |
| own-after-L1-final | 62008 | 0 |
| own-after-L2-final | 45512 | 0 |

| L2 inclusive header codec 표본 | source 전 → 후 | target 전 → 후 |
|---|---:|---:|
| active CPU sample 비중 | 15.975% → 19.389% | 11.673% → 20.690% |
| sampled heap B/message | 614.93 → 814.83 | 1020.50 → 1285.38 |

Target 표본에는 기존 protobuf reply encode가 포함되며 native payload backing bytes는 heap 표본에 없다. L1 CPU envelope 비중은 모두0이다. L1 control의 제출 수도 달라 이 수를 처리량 개선률로 해석하지 않는다. JS wrapper/native API 비용이 남으며 정식 성능 판정은 감독 측 측정이 필요하다.

8개 최종 profile/audit의 warmup·active 수량, affinity, lifecycle, loaded URL을 검사했다([raw/profile-audit.json](raw/profile-audit.json)). Warmup target는 active 전용 received 대신 allPhaseMessages를 사용했다. **초기 네 profile은 복사 도구의 경로가 n2net scratch에 남아 다른 worktree package를 읽었으므로 제외하고 전부 재수집했다. 해당 worktree의 source·build output 변경은 없었다.** 모든 raw는 보존했으며 제외 사유는 [raw/excluded-runs.json](raw/excluded-runs.json)에 있다.

## 검증과 미수정 binding 항목

| 검증 | 결과 / 근거 |
|---|---|
| native header 회귀·ownership | 같은 11개 테스트가 전 **0/11 통과** → 후 **11/11 통과**. 동기 submit/write 실패는 native close 정확히1회, 성공은 같은 Message/storage 이전. `native-regression-before-final.log`, `header-tests-final.log` |
| byte-identical | request/reply/command, deadline 유무, flow 유무, metadata, escape, correlation1/32/64B, 전체 ASCII·calendar 경계. 전 byte tests3개와 최종 header tests21개 통과. `byte-regression-before-final.log`, `header-tests-final.log` |
| focused contracts | 20 files 중 message-flow 외 **789개 통과**, 최종 message-flow **52개 통과**, 합 **841개**. 기존 allocation 예산 assertion 유지. Native Message를 Buffer로 가정한 fixture만 수정. `focused-final.log`, `message-flow-final-b.log`; 마지막 cold template 단순화 후 header21개 재검증 |
| lint / typecheck / format | 모두 rc0. `lint-final-b.log`, `typecheck-final-b.log`, `format-check-final-b.log` |
| workspace build / Unity / diff | `npm run build` rc0(`build-final-c.log`), production dist9개가 src보다 최신. 공식 build에서 Unity jspre Up to date, 변경0. git diff --check 통과 |

수신 copy 제거에는 binding 변경이 필요하므로 하지 않았다.

- **Target:** public `Message`에 native와 managed 모두 안전하게 처리하는 동기 read-only view API가 필요하다(예: `withDataView<T>(reader): T`). `message.ts:132–145,309–326`과 `message_materializer.ts:92–98`이 대상이다. Native storage를 복사하지 않고 수명을 유지하며, header를 읽었다고 managed receive 선호로 전환하지 않아야 한다. 현재 public copy().data()는 native일 때 공유할 수 있지만 managed message에서는 추가 native 복사를 만들고 공개 API로 두 상태를 구분할 수 없어 공통 해법으로 쓰지 않았다. 기존 `addon_core.cc:1113–1128,1146–1179`, `addon_message_values.h:125–137`의 native ref-share/view 구현을 재사용할 수 있다.
- **Source reply:** `completion_owner.ts:543–545`→`addon_core.cc:2138–2150`가 completion을 Framework에 주기 전에 managed Buffer로 복사한다. `RequestSubmission.reply`의 Message parts를 native-owned 상태로 전달하고 위 view로 읽을 수 있는 binding 계약/구현이 필요하다. Framework의 header reader만 바꿔서는 이 복사가 없어지지 않는다.

로컬 dependency cherry-pick: **ac4831e097** (`afd3a0cea4 -x`), **047b421385** (`ec6aa8ea5c -x`). 이 둘은 검토 중인 #4415이며 별도 owner를 만들지 않기 위해 포함했다. 본 작업 commit은 아래에 기록한다. Push·GitHub·sub-agent·다른 AI 도구·internal-docs·git fetch/pull은 사용하지 않았다. 최종 미해결 test 실패0, 수신 copy와 성능 판정은 위 한계대로 남는다.

본 작업 로컬 commit: **6f3b46c807a9147647c7b45dd758e8eaefca24e9**. Branch `perf/4352-node-l2-header`, 최종 worktree clean. 실행 중인 profile process0. 최종 검증: [raw/final-verification.json](raw/final-verification.json).
