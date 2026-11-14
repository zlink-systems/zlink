# .NET L3 service wire 진단과 수정 제안

**Send 1K의 추가 per-message CPU는 source의 `bench-submit-0..7`와 finalizer에 집중됐다.** 사다리 client는 이미 native인 service head를 다시 `Message.From(Message)`로 복제하고 원본을 dispose하지 않는다(`BC:636-642,703-710`). Production `M:10917-10933`에는 이 복제가 없다. 따라서 사다리의 18.9%를 production multipart codec 자체의 손실로 그대로 귀속하면 안 된다. Production pack은 이미 최종 native frame에 직접 쓰며, Node #4415 전의 완성 frame 재복사는 없다.

진단 HEAD `6350275fac`, branch `perf/4353-dotnet-l3-wire`. Production 변경·commit·push·GitHub 작업 없음. 공식 수치는 `4bd6102170`이며 이번 진단 HEAD와 다르다. 추출한 bench `29d9fca82b`의 Client/Server 소스는 `4bd6102170`과 같다. 공식 18.9%/9.0%와 7% 예산을 이번 공유 CPU 프로파일로 다시 판정하지 않는다.

## 근거와 포화 경계

Source/target 모두 `taskset -c 6-7,14-15`, Release, workstation GC. EventPipe cpu-sampling + verbose GC AllocationTick을 수집했다. 유효 13 cell 모두 오류 0·제출=완료=수신, SCHED_OTHER, thread affinity 일치, process lifetime 최대 **15.29초**였다. 각 process에 19.5초 hard watchdog을 뒀다. [전체 audit](raw/profile-audit.json), [호출·소유 경로](call-shape.md).

| send 1K, active 구간 | L2 | L3 |
|---|---:|---:|
| source submit 8 thread 합계 CPU / message | 12.55 µs | 14.61 µs |
| source finalizer CPU / message | 0.20 µs | 0.86 µs |
| target 수신 owner CPU / message | 4.58 µs | 4.60 µs |

L2 submit TID `2722738–2722745`, L3 `2730101–2730108`; target owner는 각각 `2722459`, `2729961`이다. Target owner는 `BT:255-338`의 단일 receive/handler loop다. Source는 `Client/BenchMeasurement.cs:365-425`의 8개 전용 submit thread이며 pack/head 생성은 그 thread에서 수행한다. Finalizer 증가 원인은 원본 head의 누락된 dispose다(`Bind/Runtime/Messaging/Message.State.cs:44-69`).

**추가 비용의 우선 원인은 source 송신·회수 경로다.** 다만 CPU 4개를 source·target·collector가 공유했으므로 단일 thread의 100% CPU 포화는 관측하지 못했다. Target owner의 runnable 시간도 약 78–91%였고 scheduler 대기가 컸다. 공식 hold에서 어느 thread가 단독 처리량 상한인지까지 확정한 결과는 아니다.

| EventPipe active stack, 호출 횟수가 아닌 sample 수 | source pack / head / 복사 unpack | target view unpack / reply pack |
|---|---:|---:|
| L2 send 1K | 0 / 0 / 0 | 0 / 0 |
| L3 send 1K | 362 / 352 / 0 | 4 / 0 |
| L2 backpressure 4K | 0 / 0 / 0 | 0 / 0 |
| L3 backpressure 4K | 87 / 55 / 399 | 3 / 140 |

Raw는 `raw/sched-L2-send1K`, `sched-L3-send1K-v2`, `sched-L2-bp4K`, `sched-L3-bp4K`에 있다. EventPipe는 대기 thread의 stack도 포함하므로 전체 sample 대비 비율을 실제 CPU 점유율로 쓰지 않았다. AllocationTick도 sampled bytes이며 객체 전수 계수가 아니다. 보조 perf의 L3 source는 새 thread 추적이 불완전해 그 sample 분모를 판정에서 제외했다.

## 메시지당 비용

`D=framework/languages/dotnet/src/Zlink.Framework`. 아래 `P=D/Runtime/Spots/ZLinkApplicationPayloadEnvelopeCodec.cs`, `S=D/Runtime/Service/ZLinkServiceWireCodec.cs`, `E=D/Runtime/Messaging/ZLinkEnvelopeCodec.cs`, `M=D/Runtime/Service/ZLinkManagedMeshNode.cs`, `Bind=bindings/dotnet/src/Zlink`, `BC=29d9fca82b:framework/bench/grpc/dotnet/Client/Program.cs`, `BT=동일 bench/ZLinkServer/Program.cs`다.

[정확한 할당 probe](raw/allocation-probe-v9.json)는 준비 호출을 제외한 4×5,000회로 관리형 B/message를 센다. 객체 수는 allocation 결과와 생성 소스를 함께 대조했다. Native storage bytes와 공통 bench payload 생성·binding submit 객체는 별도다.

| 연산 | L2 → L3 관리형 객체·bytes | 복사·native Message·검사 | file:line |
|---|---|---|---|
| source send 준비 전체, submit 제외 | **5개·280B → 11개·472B**; 1K/4K 동일 | native Message 2→5. L3가 더하는 head 2개 중 원본 1개는 finalizer까지 유지 | `BC:686-718,749-789`; `S:185-204`; `Bind/Contracts/Messaging/Message.cs:248-253` |
| service application head | +2개·64B: Message 32B + channel byte[] 32B | `bench`의 text8 6B를 임시 배열→native로 복사. Send 11B/request 19B head | `S:206-262,2289-2301` |
| multipart pack 자체 | +1개·32B, 최종 native Message 1개 | header/body를 native frame으로 각 1회 복사. 완성 frame 재복사 0 | `P:153-169,388-409` |
| 사다리 pack 인자·head 재복제 | +3개·96B: 목록 배열 40B + wrapper 24B + Message 32B | head native shared-copy 1개; 작은 inline 11/19B 값 복제. Production에는 이 추가 head 복제 없음 | `BC:643-645,711-713`; `Bind/Runtime/Messaging/Message.Native.cs:164-171` |
| count·length·validation | 관리형 할당 0 | size 순회 **2회**+write 순회 1회. 2-part는 u32 쓰기 5회(outer body/payload, count, length×2), text 길이 u8×2. 고정 profile 52B는 기존 상수 배열에서 복사 | `P:171-175,280-287,329-407` |
| target receive 전체, 실제 수신 frame+protobuf decode | 1K **1112→1144B**, 4K **4184→4216B** | 추가 객체는 channel string 1개·32B. Multipart view는 객체/배열/native Message/byte copy 모두 0. profile 비교+part bounds 순회 1회 | `S:326-348,2877-2919`; `P:211-257,490-540`; `E:14-85` |
| reply service head·pack | head 2개·80B; pack 1개·32B; 사다리 pack 인자 2개·64B | reply head byte[]→native 21B 1회. Multipart는 위와 같은 writer. L2 response 생성과의 차이는 아래 참조 | `S:436-473`; `BT:438-457`; `M:12365-12384` |
| 사다리 completion unpack | +3개·104B, native Message +2개 | bounds 순회 후 두 번째 순회에서 packed native frame→새 part native storage 2회·총 1,228/4,300B. 관리형 payload 배열 복사는 아님 | `BC:824-854`; `P:436-487`; `Bind/Runtime/Messaging/Message.Native.cs:328-336` |
| production completion unpack | 추가 관리형 할당·part 복사 0 | 원래 reply frame을 record가 유지하고 view로 읽음 | `M:11442-11499` |

실제 send에서 inner header는 252B, protobuf body는 1,027/4,099B다. L3 pack의 추가 inner 복사는 **2회·1,279/4,351B**, 최종 payload frame은 **1,354/4,426B**다. Message의 native block 생성·close는 고정 비용이고, payload copy는 크기 비례 비용이다. 이 둘을 합쳐 “완성 frame을 두 번 복사한다”고 진단하지 않는다.

Request 사다리에는 비용 비교를 왜곡하는 차이도 있다. L3 reply는 `BT:451-457`에서 marker·correlation 없는 고정 `RawEnvelopeHeaders.Response`를 넣고, source `BC:824-854`는 inner response header를 parse하지 않는다. L2의 `BT:344-365` 동적 response encode와 `BC:807-808` parse를 유지하지 않는다. 반면 L3 completion은 production에 없는 복사 unpack을 추가한다. **Backpressure 9%를 순수 framing의 비용으로 분해할 수 없다.**

## 언어별 대조

C++/Java는 진단 HEAD 기준, Node는 **`ec6aa8ea5c`(#4415)**를 `git show`로 읽었다. `C=framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp`, `J=framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java`, `N=framework/languages/node/packages/framework/src/runtime/foundation/service-wire-m6a-codec.ts`다.

| 항목 | .NET | C++ | Java | Node #4415 |
|---|---|---|---|---|
| frame pack·최종 native 저장소 | `P:153-169`, Message 1개 | `C:59-106,3224-3228`, native writer | `J:383-406`, Message.allocate | `N:282-298`, owned native frame |
| part count/length encode | **`P:174,341,388-409`: size 재합산 1회 추가** | `C:3168-3175,3193-3202`: size 1회+write | `J:443-463`: size 1회+write | `N:312-340`: size 1회+write |
| inner copy / 완성 frame 재복사 | `P:407`: part당 1회 / 0 | `C:3194-3202`: part당 1회 / 0 | `J:459-463`, `ZLinkFrameworkMultipartView.java:122-130`: part당 1회 / 0 | `N:290-293,337-338`: part당 1회 / 0 |
| service head text | **`S:233-235,2289-2301`: 임시 UTF8 배열** | `C:571-587`: string bytes를 native에 직접 기록, 임시 UTF8 배열 없음 | `J:253-267,281-291`: 임시 channel bytes 있음 | `N:135-156`: 길이 계산 뒤 최종 header에 text 기록 |
| receive unpack | `P:490-540`, `E:14-85`: 2-part inline value view, 복사·객체 생성 0 | `C:3383-3428`: retained owner+external Message views, byte copy 0 | `J:508-551`: Object[]+ByteBuffer slices+view, byte copy 0 | `N:433-451,492-529`: 배열+Buffer slices+wrapper, byte copy 0 |
| reply pack·head | `M:12378-12383`: 같은 pack; **head는 byte[]→Message** | `runtime/mesh/raw_mesh_node_owner.cpp:2151-2155`, `C:3913-3919`: native head+같은 pack | `runtime/binding/ZLinkJavaRawMeshNode.java:5415-5430`, `J:306-312`: byte[] head+같은 pack | `N:185-203,282-298`: 작은 Buffer head+native payload |
| completion unpack | production `M:11479-11491` view; **사다리만 `P:474` 복사** | `C:3400-3428`: retained frame | `runtime/binding/ZLinkJavaRawMeshNode.java:5471-5474`: retained frame view | `runtime/backend/node/node-raw-mesh-backend.ts:2566-2571,2966-2967` + `N:517`: slices |

.NET의 작은 Message는 managed wrapper와 native message block 생성·해제를 함께 요구한다(`Bind/Runtime/Messaging/Message.State.cs:19-24,49-69`). C++의 native message 값에는 이 managed wrapper/finalizer 비용이 없다. 다만 Java/Node도 객체·native handle 비용을 내므로 “다른 세 언어는 모두 무할당”이라고 보지 않는다. .NET receive view는 오히려 2-part에서 객체를 만들지 않는다.

## 수정 제안과 예상 효과

**A안: 기존 codec의 중복 작업을 제거하고 소유자는 유지한다. 권장한다.** B안은 C++ `runtime/messaging/envelope_codec.cpp:565-615`, `C:3262-3272`처럼 inner header writer를 final multipart frame에 직접 연결해 임시 header Message까지 없애는 것이다. B안은 내부 envelope 표현과 deferred snapshot 수명을 함께 바꿔야 하며, 이번 증거만으로 그 확대를 요구하지 않는다.

| 파일·함수 | 구체적 변경 | 메시지당 기대 효과 |
|---|---|---|
| `P`, `WriteFrameworkMultipartEnvelope` 두 overload | 이미 계산한 encodedLength에서 기존 envelope overhead를 빼 payload length를 얻음. 기존 `WriteMultipartPayload` 유지 | size 순회 2→1, prefix·byte copy·Message 1개·32B 유지 |
| `S`, `PrepareApplication/ApplicationLength/WriteApplication`, 기존 text helper | text 검증·길이 계산은 기존 한 곳에 유지하고 UTF8를 최종 destination span에 기록. byte[] encoder와 native encoder가 같은 writer 사용 | `bench` head **2개·64B→1개·32B**, 임시 text8 6B copy 1→0. Native Message 수는 1→1 |
| `S`, `EncodeReply`; `M`, `PrepareNativeTerminalReply` | 기존 reply 검증·field writer를 공유하는 내부 native Message encoder로 호출 연결. Byte 소비자도 동일 writer 사용 | head **2개·80B→1개·32B**, 임시 배열·21B copy 제거. 아직 실험으로 구현하지 않은 예상치 |

사다리의 별도 교정 범위는 `BC`의 head 복제 제거·생성 실패 시 head close·기존 `payloadParts` 재사용, `BT`의 L2 response 생성 유지, completion의 production view+inner header parse다. 이는 codec의 손실을 숨기는 변경이 아니라 production 호출 경로와 단계 정의를 맞추는 변경이다. Production 수정으로 이 사다리 결함을 보상하지 않는다. 이 교정도 현재 작업에서는 제안만 한다.

규칙 수는 해당 hot path의 실제 판정/보관 위치를 센다. [네 언어 결정 소유 표](call-shape.md#결정-소유-표).

| 결정·소유 | 현재 위치 | 전→후 |
|---|---|---:|
| multipart 길이 합산 | `P:174,341` (Message 입력) | 2→1 |
| count·part length·part bytes writer | `P:388` | 1→1 |
| 최종 packed buffer 생성 | `P:158` | 1→1 |
| pack 실패 close | `P:166`; 성공 뒤 기존 record/binding owner | 1→1 |
| application head byte 저장소 | `S:233`의 임시 text 배열 + `S:193` native frame | 2→1 |
| reply head byte 저장소 | `S:463` 배열 + `M:12378` native Message | 2→1 |

Wire·bounds·UTF8 validation owner는 기존 codec이다. 새 flag·queue·lock·cache·timer·routing/retry 판단은 없다. 제거되는 것은 임시 text/reply 배열이며 view·completion 수명은 바꾸지 않는다.

## Scratch 실험과 검증

[실험 결과](raw/scratch-assertions.json): 독립 wire fixture **16개**, service head **24개** byte-identical; malformed frame **80개**·잘못된 text **4개** 거부; pack snapshot·수신 view backing identity 통과. 할당 검사 `≤32B/head`는 기존 **64B로 실패**, 제안 writer **32B로 통과**한다. 계측한 size 순회는 **2→1**이다. 모두 `/root/worktree/zlink-v21-s033/d3l3-scratch`의 실험이며 production 구현이 아니다.

[CPU 보조 probe](raw/cpu-probe-summary.json)는 tiered compilation off, 연산별 4×100,000회이며 process CPU에 finalizer도 포함한다. Source send 준비는 1K **750→2200ns**, head 복제만 없애면 **1300ns**였다. 4K는 **725→2225→1450ns**였다. Target 실제 receive frame의 decode는 1K **200→325ns**, 4K **500→625ns**였다. Source의 증가가 payload 크기와 거의 무관한 고정 비용이라는 근거다. Single size-pass의 CPU 이득은 1K에서 검출되지 않았고 4K도 400→375ns 수준이므로 큰 처리량 회복을 약속하지 않는다.

향후 production 검사는 기존 `EnvelopeCodecAllocationRegressionTests`, `MeshOutboundPayloadOwnershipTests`, `MeshReplyPayloadOwnershipTests`, wire conformance 시험에 연결한다. byte[]/native encoder의 독립 golden bytes, UTF8 경계·malformed input, 정상/동기 실패/취소의 단일 release, 32B head 할당 회귀를 고정한다. 새 검사는 기존 구현에서 실패하는 음성 대조군도 유지해야 한다.

Release bench 4 build와 scratch 2 build 통과, DLL source freshness·Core 1.25.0 SHA256 `85f228638e89…` 확인. [최종 검증](raw/final-verification.json). 초기 trace 옵션 오류·thread 종료 조회 race·stale DLL probe 실패는 삭제하지 않았고 판정에서 제외했다. Guard의 T 상태는 기다렸으며 CONT를 보내지 않았다. 남은 유효 실험 실패는 없다. **공식 throughput·7% 재판정은 남아 있다.**

소유 계층: Framework service wire codec; 사다리의 Message 해제는 bench caller.

Spec: `02-channel-transport/06-wire-protocol.ko.md:185-215`, `01-execution/05-payload-ownership-and-codec.ko.md` §1–6. 변경 없음.

교차언어: 네 언어 모두 native 최종 frame·receive view를 제공한다. .NET의 재합산·임시 head 배열과 사다리의 복제/복사 차이를 확인했다.

분류: **B(기존 내부 비용·사다리 소유 결함)**. Spec/public API 변경 제안 없음.

점검: 발견 7건(사다리 4, production 비용 3), production 수정 0, PM 검토 대상 7건, 별도 이관 0.
