# Node L2 header 비용과 production 수정

Node 경로는 `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts`, .NET 경로는 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs`다. 아래 `N`, `D`는 이 두 파일이다. Node line은 최종 source, .NET line은 69891f4446과 동일한 source다. 수정 전 Node는 5f3a59bff6이다. 구현 전 비교표·소유 표·제안과 후보 개정은 `raw/report-proposals.md`에 보존했다.

| L2 비용 | Node file:line / caller | .NET file:line / caller | Node가 더 하는 작업과 .NET이 덜 하는 작업 |
|---|---|---|---|
| source request header encode | N:130,162,477,572,670; `channel-outbound-operations.ts:212-222` | D:188,1195; `ZLinkClientCallCodec.cs:7-36` | 전: ID 문자열 연결/flatten과 Date 객체를 매번 생성. 최종: ID는 counter bytes→hex 1회, Date scratch 재사용. .NET은 ID string 1개와 value-type timestamp를 쓰며 prefix bytes를 재사용 |
| target request header decode | N:890,927,964,1056,1628; `channel-dispatchers.ts:145,268` | D:377,414,933; `Runtime/Channels/ZLinkClientServerDispatcher.cs:140,247` | Node는 reader object 1개+deadline string을 생성. .NET Utf8JsonReader는 value type이며 deadline은 TryGetDateTimeOffset으로 읽음. 둘 다 필요한 channel/message/content-type/correlation 값은 string으로 만듦 |
| target reply header encode | N:223,477,670; `channel-dispatchers.ts:409-412` | D:177,188,1195; `Runtime/Channels/ZLinkChannelReplyWriter.cs:14,34-60` | Node는 전체 template를 복사 후 correlation/flow를 덮어씀. .NET은 prefix만 복사하고 dynamic field를 최종 native Message에 씀. Node reply의 protobuf encode는 header-only 비용이 아님 |
| source reply header decode | N:327,890; `channel-outbound-operations.ts:243-248` | D:377,414,933; `ZLinkClientCallCodec.cs:60-91,125-126` | Node reader 1개와 비-null 값 string 4개. .NET 실제 L2 reply는 messageName이 비어 있어 비어 있지 않은 값 string 3개+Header record 1개 |
| correlation 생성·검증 | N:118,1695,1723,1731 | `Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamCorrelation.cs:9-13`; D:1097-1106 | 전: suffix hex, zero slice, 연결 2개, flatten까지 string 5개. 최종: hex string 1개. 외부/수신 ID의 1..64 ASCII 검증은 유지. .NET은 짧은 counter hex string 1개; Node의 기존 32자 형식은 유지 |
| deadline formatting/parsing | N:527,541,1056,1628 | `ZLinkClientCallCodec.cs:33`; D:497-506,1229-1230,1363-1380 | 전: Date 1개+ISO string 1개/요청. 최종: per-call Date 0개, ISO string 1개는 유지. .NET은 DateTimeOffset·stackalloc·Utf8Formatter로 heap Date/ISO string 없이 처리. Node L2 decoder는 deadline string을 반환하며 재parse하지 않음 |
| metadata / flow | N:1102,1628,1754-1808 | D:518-537,588-635,943-1023,1251-1277 | Node canonical empty metadata는 shared record로 object allocation 0. .NET은 field가 오면 빈 경우도 Dictionary를 만들지만 자기 wire에서는 null metadata를 생략. Node tracing Off는 flow 값 string 생성도 생략. Metadata 값/flow validation은 변경 없음 |
| field 이름 / JSON 검사 | N:927,964,1056,1334 | D:414,637-670 | .NET known field 이름은 stack char buffer로 비교. Node canonical/일반 known 이름도 bytes로 비교함. **이미 전체 header string/JSON.parse 재검사는 없다.** Node escape가 있는 string token만 JSON.parse를 사용. 따라서 전체 JSON.parse 제거를 이번 개선의 근거로 쓰지 않음 |
| outbound buffers / copies | N:670-725; binding `native/src/addon_core.cc:211-222,904-924` | D:1195-1236,1280-1288 | Node request/reply: header Buffer view 1개+slab 일부, template 전체 copy 1회, binding native storage로 copy 1회. .NET: native Message storage 1개에 prefix copy 1회+dynamic write, 중간 JS Buffer 없음. command(no flow)는 Node shared bytes, encoder allocation/copy 0 |
| inbound buffers / copies | N:890; binding `runtime/sockets/socket_operations.ts:226`, `runtime/messaging/message_materializer.ts:92-98`; native `addon_message_values.h:248-265`, `addon_core.cc:509-525,2138-2150` | D:377-378 (`Message.AsReadOnlySpan`) | Node target는 header까지 읽으면 다음 receive부터 managed-buffer 경로를 골라 header Buffer 1개를 만들고 native→managed copy 1회. 첫 native-frame 경로도 `contracts/messaging/message.ts:309-326`에서 읽을 때 copy. Source completion도 reply header Buffer/copy 1회. .NET decoder는 native span을 읽어 추가 header byte copy 0 |
| strings / header message | N:118,541,1056 | D:486-506,578-582,637-670 | 생성 request: 전 ID5+ISO1→최종 ID1+ISO1. Decode request: Node5 / .NET4; reply: Node4 / 실제 .NET L2는3; command3. Flow를 읽으면 +1, metadata entry마다 key/value +2. Escaped token은 Node JSON source string도 더 생김 |

N의 caller 약칭은 같은 `runtime/channels/`, D의 caller 약칭은 같은 `Runtime/Messaging/` 기준이다. Binding 상대 경로는 `bindings/node/src/zlink/` 또는 `bindings/node/native/src/`다. Node binding의 설치된 JS도 copyData 경로를 확인했으며 hash는 `raw/final-code-hashes.json`에 있다.

## 원인과 수정 범위

잔여 L2 손실은 request/reply header encode·decode가 event loop에 더하는 동기 CPU와 allocation 비용이다. 이번 조사에서 pending 누수·reply 유실·새 poller/await hop의 증거는 없었다. Node는 .NET보다 임시 ID string, per-request Date, reader/deadline string과 native↔managed header copy를 더 만든다. 이번 production 수정은 Node codec이 소유한 제거 가능한 임시 표현과 중복 Buffer 인자 검증을 줄였다.

실제 L2 entry도 다르다. Node `2bef18aa94:framework/bench/grpc/node/shared/raw-wire.js:89-116`은 full envelope-parts entry를 부르고 source에서는 constant empty serializer로 header만 얻는다. 따라서 source의 payload selection/packet-name 확인/parts array도 이 비용에 들어간다. .NET `69891f4446:framework/bench/grpc/dotnet/Client/Program.cs:436-449`는 CreateEnvelope→EncodeHeader만 부른다. Target .NET은 `ZLinkServer/Program.cs:226-243,283-304`의 DecodeHeader→EncodeRawBodyParts이며, Node reply encode profile에는 실제 protobuf encode가 포함된다. 서로 다른 profile 경계를 순수 JSON header 비용으로 혼동하지 않았다. Scratch bench는 지정 커밋 그대로이며 bench-only 수정은 없다.

.NET PrepareResponse(D:259-274)는 빈 messageName을 준비하고 null metadata를 생략한다. Node가 이 표현을 그대로 따르면 byte output과 decoded values가 달라지므로 적용하지 않았다. 수신 copy도 binding 소유다. Framework에서 private native frame을 읽거나 새 public API를 추가해 우회하지 않았다.

역사적 reference: `lad-node-s1/report.md`의 L1→L2는 1K 30,055→20,515(31.74%), 4K 25,745→18,708(27.33%) 손실이다. .NET은 `lad-dotnet-s1/progress.md:133-177`의 71,957.2→62,976.2(12.48%), 83,673.6→75,668.6(9.57%)다. 해당 문서의 raw 링크가 근거이며, 이번 제한 CPU profile로 이 throughput 손실률을 다시 판정하지 않는다.

## production 변경과 결정 소유

- N:99,112-127: random prefix와 64-bit counter를 같은 16-byte buffer에 보관하고 carry로 증가시킨 뒤 hex string 하나를 반환한다. Scalar counter·zero slice·문자열 연결/flatten을 제거했다. 기존 32자 lowercase hex 형식과 random 64-bit prefix를 유지한다.
- N:114-116,527,541-545: 동기 codec의 Date scratch 한 개를 매번 setTime 후 ISO로 변환한다. per-request Date를 제거하고 native ISO의 calendar·범위·오류 규칙을 재사용한다. Date scratch는 operation deadline cache가 아니며 값을 매번 입력으로 덮어쓴다.
- N:723-725: 길이가 이미 고정된 prepared correlation/deadline/flow는 공개 `Buffer.write(string, offset, 'ascii')` overload를 쓴다. 별도의 length 재검증을 제거했다.

| 결정 | 스펙 / 네 언어의 현재 위치 | 최종 소유자 |
|---|---|---|
| header encode/decode/validation | ClientServer §5.3, hot path E1/I3/W4. Node N:572,890,1628; .NET D:188,377,933; C++ `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:168,527,1293-1328`; Java/Kotlin 공유 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:502,634,846,1213` | 각 Framework codec 그대로 |
| correlation 생성 | Node N:118; .NET `Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamCorrelation.cs:13`; C++ `runtime/messaging/client_call_codec.cpp:16`; Java/Kotlin `runtime/internal/service/ZLinkServiceOperationIds.java:17,30` | Node 기존 generator 한 곳 |
| deadline ISO | Node N:541; .NET D:1363; C++ `client_call_codec.cpp:34-61`; Java/Kotlin 공유 channel codec | native formatter를 호출하는 기존 codec |
| native/managed byte 소유 | Node binding `addon_message_values.h:248-265`, `addon_core.cc:211-222`; .NET binding Message와 D:377 | binding, 이번 수정 없음 |

소유 계층: **Framework channel-envelope codec**. Binding completion/admission·receive mode·queue·execution gate는 바꾸지 않았다.

스펙: `framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md` **§5.3**, `01-execution/08-messaging-hot-path.ko.md` **§3 E1 / §4.1 I3 / §4.3 W4**.

교차언어 대조: .NET value-type reader/formatter, C++ retained view와 native writer, Java/Kotlin byte reader를 대조했다. 유효 header의 공통 계약을 유지하는 Node 내부 표현 변경이다. 다른 언어의 wire 표현을 Node에 적용하지 않았다.

변경 분류: **B — 기존 내부 비용 수정**. public API·wire format·spec 변경 없음.

규칙 수(판정 위치): correlation 생성 1→1, deadline 값/ISO 결정 1→1, wire/correlation 검증 1→1, prepared-field 길이 판정 2→1(eligibility와 Buffer length 인자→eligibility). 새 queue/lock/flag 없음. Prefix 문자열+Number counter를 byte storage로 합쳤고, 공유 Date scratch는 per-call Date를 대체한다.

대안: 직접 Gregorian 계산은 최적화 모드에서 allocation을 줄였지만 interpreter 숫자 boxing이 증가해 기존 allocation gate를 실패했다. 정수 cast도 같아 기각했다(`raw/alloc-after`, `raw/int-probe`). 최종안은 calendar 규칙을 추가하지 않는다. 새 회귀 예산은 final array+element storage(2), interpreter setTime boxed result(1), ISO string(1)이며 **기존 roundtrip assertion은 그대로 유지했다**.

## allocation / copy 계수

같은 production package를 연결한 `n2net-scratch`에서 2,000회씩 V8 exact allocation trace를 수집했다. 실제 Date/ID와 header 작업을 포함하되 body codec은 같은 constant encoded payload를 반환한다. ArrayBuffer 외부/native payload bytes와 binding 송수신 Buffer는 아래 codec-only 숫자에 포함하지 않고 위 표에 별도 계수했다. `.NET`은 code reading으로 계수했다.

| header 연산 | optimized V8 전 → 최종 (allocations / B per message) | interpreter 전 → 최종 (allocations / B per message) |
|---|---:|---:|
| request encode | **10.101 / 487.50 → 5.101 / 271.50** | **21.101 / 903.50 → 14.101 / 591.50** |
| reply encode | 2.081 / 166.05 → 2.081 / 166.05 | 9.081 / 422.05 → 7.081 / 342.05 |
| request decode | 7.189 / 406.83 → 7.565 / 422.33 | 17.012 / 744.49 → 17.012 / 744.49 |
| reply decode | 6.370 / 372.49 → 6.343 / 370.92 | 14.011 / 632.47 → 14.011 / 632.47 |
| command encode | 1 / 64 → 1 / 64 | 2 / 80 → 2 / 80 |

근거: `raw/alloc-jit-stable-before/allocation-summary.json`, `raw/alloc-jit-stable-final/allocation-summary.json`(50,000 warm calls), `raw/alloc-before/allocation-summary.json`, `raw/alloc-final/allocation-summary.json`(5,000 warm calls). 각 `.log`는 module SHA256·warmup·call count를 기록한다. Interpreter 수치에는 Node validation wrapper의 rest arguments와 primitive number boxing도 포함된다. Optimized trace는 JIT bookkeeping·inlining에 따른 소수 변동이 있다. **Decoder는 source를 변경하지 않았으며, interpreter 대조도 동일하다.**

정상적인 optimized request의 차이는 ID string 5→1과 Date object 1→0이다. ISO string·boxed timestamp·final parts array·header Buffer는 남는다. Slab backing 교체를 분할한 약 0.101 allocations/message도 포함했다. Reply에서 제거한 validation wrapper allocations는 optimized V8에서 이미 제거되므로 optimized object 수는 같다.

| .NET header 연산 | managed allocations / copy (payload 제외) | 근거 |
|---|---|---|
| 실제 L2 source encode | Header record1 + correlation string1 + native Message wrapper1 = **3개**, native header storage1. Parts wrapper를 만들지 않음 | `Client/Program.cs:443-449`; `ZLinkClientCallCodec.cs:28-36`; D:1203 |
| 일반 production EncodeParts | 위 3개 + TwoMessageParts1 = **4개** | D:157-172; `ZLinkMessageParts.cs:25-29,69` |
| L2 target reply encode | reply Header record1 + Message wrapper1 + parts wrapper1 = **3개**, native storage1. 새 correlation/deadline string0 | `ZLinkChannelReplyWriter.cs:41-60`; `ZLinkServer/Program.cs:291-304`; D:177-181 |
| 실제 L2 request decode | Header record1 + 값 string4 = **5개**, reader/deadline/known-field-name heap allocation0 | D:414-582,637-670 |
| 실제 L2 reply decode | Header record1 + nonempty 값 string3 = **4개**. 빈 messageName은 String.Empty | D:259-274,414-582 |
| metadata / flow가 있으면 | metadata Dictionary1+storage, key/value string 각1/entry; flow string1. Metadata field가 없으면 Dictionary0 | D:518-537,588-635 |

Node representative wire는 request272B / reply213B이며 전후 같다. Outbound 각각 template full copy1 + native submit copy1, inbound 각각 native→managed header copy1이다. .NET은 native output에 prefix1회 복사하고 dynamic field를 쓰며 decoder span 추가 copy는0이다. 이 계수는 Core TCP/framing의 공통 복사와 body protobuf 복사를 제외한다. Node header copy를 없애기 위해 현재 Message 계약 밖의 native 접근/API를 추가하지 않았다.

## 실제 L1/L2 profile

Ladder `2bef18aa94`의 Node bench/proto를 **git archive**로 `/root/worktree/zlink-v21-s033/n2net-scratch`에 만들고, 이 branch의 production package를 연결했다. 다른 worktree나 그 build output은 사용하지 않았다. 새 build와 source/output freshness, native addon/Core SHA256은 run별 `raw/*-fingerprint.json`에 기록했다. Native hashes는 l2bp와 동일했다.

모든 process/thread CPU **6–7,14–15**, start/mid-active policy **SCHED_OTHER**. 각 cell 전 discarded warm run, warmup3s+active4s, `--cpu-prof` 500µs + collected-object allocation sampling32KiB. Source/target 초기화는 동시에 시작하며 모든 전후 유효 run에 같은 runner를 썼다. child hard timeout19s+kill-after1s. measure.lock 미사용. 준비 실패와 기각 후보 raw도 삭제하지 않았다.

| envelope inclusive sample 비중 | L1 전 / 후 | L2 전 → 최종 |
|---|---:|---:|
| source 전체 | 0 / 0 | **17.400% → 15.149%** |
| target 전체 | 0 / 0 | **7.225% → 9.603%** |
| source request encode | — | 13.998% → 12.209% |
| source reply decode | — | 3.345% → 2.772% |
| target request decode | — | 2.773% → 3.730% |
| target reply encode (protobuf 포함) | — | 4.451% → 5.813% |

근거: `raw/node/{before,after}-L{1,2}-final/cpu-summary.json`. l2bp와 같은 전체 active sample 분모를 썼다. Idle만 뺀 envelope 비중도 source17.636%→15.306%, target7.866%→10.409%다(`cpu-denominators.json`). **Target 비중은 증가했으며 전체 CPU/throughput 개선이나 약29% 손실 해소를 주장하지 않는다.**

실제 ladder의 codec 하위 inclusive allocation sampling은 source **838.03→665.09 B/message**, target **1022.58→1069.94 B/message**다(`raw/node/{before,after}-L2-final/heap-summary.json`). Warmup+active source 제출 수로 나눈 sampling 추정치이며 target는 protobuf와 그 wrapper도 포함한다. Native payload backing bytes는 포함하지 않는다. Header-only exact trace와 구분한다.

동일 process에서 old/new codec을 80ms window로 8회씩 순서를 바꿔 실행한 보조 CPU probe는 request encode after/before0.704, reply1.052, request decode0.955, reply decode1.053, command control1.062였다(`raw/paired-codec/summary.json`, windows·CPU profile 보존). Decode와 command control도 흔들리므로 정확한 개선율로 쓰지 않는다. Source encode 비용 감소를 보조하며 target 개선을 입증하지 않는다. 실행은19s hard timeout 안에서 종료했다.

| 유효 run | active 제출 = 완료 = 수신 | errors | 전체 lifecycle |
|---|---:|---:|---:|
| before-L1-final | 30,440 | 0 | 9.475s |
| before-L2-final | 27,635 | 0 | 11.086s |
| after-L1-final | 68,280 | 0 | 8.716s |
| after-L2-final | 21,029 | 0 | 9.517s |

각 run의 stats·CPU/heap raw·thread snapshots와 `raw/profile-audit.json`이 근거다. 이 수를 msg/s 개선률로 해석하지 않는다.

## 검증과 남은 항목

| 검증 | 결과 / raw |
|---|---|
| Byte-identical header / decoded values | request/reply/command, deadline 유무, flow·metadata·escape, 모든 ASCII 및64-byte correlation, 잘못된65-byte/비ASCII, calendar/확장 year/Date 최대범위. 기존 codec 새 byte tests3개 통과(`header-bytes-before.log`), 최종 bytes+기존 boundaries9개 통과(`header-tests-final.log`) |
| 양방향 allocation 회귀 | 기존 codec은 최종 새 budget4.25개를 **7.002개/296.088B로 실패**, 최종은 **4개/136B로 통과**. `cost-regression-before-final-budget.log`, `cost-regression-final.log`. 기존 roundtrip13.5개/1100B assertion 유지 |
| 관련 framework tests | 13 files **273 tests 통과, 최종 실패/skip0**. `focused-final.log`의12 files221개와 build metadata 보완 후 `message-flow-final.log`52개. 첫 message-flow 실패는 own dist/package.json 미생성으로 인한 ESM/CJS import 실패였고 source/expectation 수정 없이 공식 build 준비와 같은 manifest를 생성함 |
| Node format | `scripts/format/format.sh --check node` 통과 (`node-format-final.log`) |
| lint / typecheck | `npm run lint`, `npm run typecheck` 통과 (`node-lint-final.log`, `node-typecheck-final.log`) |
| fresh build / diff | own `tsc -b packages/framework` 성공; `git diff --check` 통과. Fingerprints와 `final-code-hashes.json` 보존 |

변경 파일: `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts`, `framework/languages/node/test/contract/node-envelope-codec-cost.test.js`, `framework/languages/node/test/contract/node-envelope-header-bytes.test.js`.

리팩토링 점검: 비용/중복 검증3건(ID 임시 문자열, per-call Date, prepared field length 인자)를 수정했다. Execution-gate 중복 동기화 발견0. 미수정 차이: Node의 inbound/outbound header copy와 reader/deadline string, .NET L2의 다른 entry/wire 표현. Binding/API 설계로 확대하지 않았다. 남은 test 실패0, 정식 throughput/잔여 손실률 재판정 없음. raw와 scratch는 보존했다. local commit만 수행하며 push·PR·GitHub·sub-agent·internal-docs 사용 없음.
