# Java L2 header 비용 수정 후보

**완료 판정: 보류.** 할당·중간 복사는 줄였으나, 같은 JVM 교대 probe의 encode CPU가 증가했다. 7% 예산 충족을 주장하지 않는다. 최종 package 검사는 지정 `test-slot.sh`의 `measure.gate`에서 대기하다 시간 상한으로 취소했다. 이전 전체 package 검사는 4,146개 통과했으며, 최종 native primitive writer는 아직 그 검사를 통과한 것으로 판정하지 않는다.

Branch `perf/4352-java-l2-header`, 시작 `2125999e62`. GitHub·push·sub-agent·internal-docs·fetch/pull·measure.lock 직접 획득 없음. Raw·scratch 보존. Bench는 `f3e3933567` archive와 같은 commit의 공용 proto를 사용하고, 이 worktree Framework에 composite build했다. JAR SHA-256은 각 실행의 `*-jars.json` 및 `raw/final-fingerprint.json`에 기록했다. 최종 core JAR: `bd1362cfa9767268c5d980126a8beccf266325faeae787806530a72f05370cc9`.

## Per-message 비교

아래 Java `J`는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java`, `I`는 같은 core의 `runtime/internal/service/ZLinkServiceOperationIds.java`, `V`는 `runtime/internal/service/ZLinkFrameworkMultipartView.java`다. .NET `D`는 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs`, `C`는 같은 디렉터리의 `ZLinkClientCallCodec.cs`, `R`은 `Runtime/Channels/ZLinkChannelReplyWriter.cs`다. Java 번호는 최종 후보이며 전/후 비용은 같은 wire 값을 기준으로 한다.

| 단계·비용 | Java 전 → 후보 / file:line | .NET / file:line | 판정 |
|---|---|---|---|
| client request 생성 | Header 1개, UUID identity, hex String 2개+연결 → Header/identity 유지, 16-byte 입력으로 hex String 1개. `J:201`, `I:30` | Header record 1개, counter hex String 1개. `C:26`, `Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamCorrelation.cs:13` | 중간 String 2개 제거. 32자리 소문자·leading zero 동일 |
| request header encode | size/write 두 순회. Heap header 배열+native copy → 최종 Message에 직접 기록. `J:578,642` | size/write 두 순회, 최종 native Message span. `D:1195,1217` | staging 배열·복사 제거. Java public Message primitive write의 반복 bounds 검사 CPU는 남음 |
| field 이름·등록값 출력 | 필드명 char loop와 prepared 값 copy → 같은 HeaderField 상수에서 null 포함 토큰, 등록 시 전체 field encoding 준비. `J:54,321` | cached HeaderPlan prefix + 동적 값. `D:1217,1280` | 새 cache 없음. Java는 독립 등록 이름들의 토큰을 여러 번 기록함 |
| target request parse | HeaderReader 88 B + CanonicalReader 32 B, canonical miss 시 Jackson·UTF-8 재검사 → reader 32 B 하나, UTF-8·escape·unknown·중복 검사 통합. `J:1000,1044,1196,1349` | value-type Utf8JsonReader, 이름 stack buffer, 값 String, Header record. `D:414,637,544` | reader scaffold 120→32 B. 일반 JSON도 같은 header parser 사용 |
| parse String·copy | ASCII 비-null 값마다 독립 String+backing array, native scratch 1개 유지. `J:1349` | 비어 있지 않은 값은 String, span에서 읽음. `D:461–506` | native scratch 최대 64-byte 초기 배열은 잔여 JVM 비용. 정상 known field 이름 String 0개. escaped/일부 unknown 이름은 String을 만듦 |
| metadata·flow | empty Map 공유. nonempty LinkedHashMap→immutable Map, flow Off는 값 생성 없이 문법만 검사. `J:1044,1240` | metadata Dictionary, flow 검증, Header record. `D:518–635,943` | Map 의미·UTF-8 크기·중복·flow 계약 유지. Decode cache lookup 정책 유지 |
| deadline | 없으면 String 0개. 있으면 제공받은 문자열 encode/decoded String 유지. `J:578,1044` | DateTimeOffset value, UTF-8 formatter/reader. `D:497,1229,1363` | L2 bench는 deadline 없음. Java의 문자열 반환 형태를 바꾸지 않음 |
| target reply | reply Header 1개, correlation/flow/name 참조 재사용, 위 writer 1회. `J:227`; `runtime/channels/ZLinkChannelDispatchReporter.java:378` | reply Header record, prepared response, 위 writer. `R:34`, `D:188` | Java의 기존 messageName echo 유지. .NET Response의 빈 messageName과 wire가 달라 String 수가 같지는 않음 |
| client reply/completion | Header parse 1회 뒤 body decode. 물리 pending 제거·retained 해제 뒤 future 완료. `runtime/channels/ZLinkChannelCallRuntime.java:260`; binding `runtime/sockets/CompletionOwner.java:1199–1229` | `C:60–126`; binding `Runtime/Messaging/CompletionOwner.cs:1220,1322` | binding completion·correlation·admission 변경 없음. 정상 profile의 codec 오류 0 |
| multipart 소유 | 기존 Object[2]+view, cache bytes 또는 Message와 body owner 유지. `V:51,56,77,221` | inline TwoMessageParts 1개. `Runtime/Messaging/ZLinkMessageParts.cs:25,66` | Java Object[2]는 잔여 차이. 이번 변경에서 별도 pair class/상태를 추가하지 않음 |

## 결정 소유와 규칙 수

소유 계층은 Framework envelope codec이다. 기준은 ClientServer Channel §5.3 header grammar/structure와 messaging hot path I3/W4다. Java/Kotlin은 같은 Java codec, C++ `framework/src/runtime/messaging/envelope_codec.cpp:527,1320`, .NET `D:188,414`, Node `runtime/channels/channel-envelope.ts:886`을 대조했다. 값·wire·오류 계약 변경 없이 내부 비용을 줄이는 **B** 후보이며, spec·binding 수정은 없다. 구현 전 소유 표·대안 비교는 `progress.md`에 있다.

| 판정 위치 | 전 | 후보 |
|---|---:|---:|
| header writer | 1 | 1 |
| header parser | 2 | 1 |
| 완성된 field/metadata/flow 의미 검증 | 1 | 1 |
| output buffer 소유자 | 1 | 1 |
| physical completion 소유자 | binding 1 | binding 1 |

새 flag/queue/lock/cache/timer/ThreadLocal 없음. HeaderReader의 값·검증 상태와 CanonicalReader를 제거하고, cursor·기존 scratch·flow cache exclusion만 단일 reader로 이전했다. HeaderField는 wire token 상수와 파싱의 단일 정의이며 cache entry가 아니다. 리팩토링 점검: 중복 parser, 중간 문자열, staging copy 3건 수정 후보; execution-gate 중복 동기화 발견 0. 잔여 차이는 위 표에 명시했다.

## Cold·steady profile

전 `raw/before-v2-*`, 후 `raw/final-*`. 모든 JVM/스레드 CPU `6-7,14-15`, hard timeout 30초. 기록은 JVM 시작부터, 분석은 workload start 후 첫 2초와 15초 warmup 뒤 5초다. AP 4.6 ctimer 1ms, allocation 256 KiB sampling, PrintCompilation/LogCompilation. 공식 1초 warmup 측정 조건은 변경하지 않았다.

아래 B/msg는 envelope/identity 하위 allocation 표본을 source 제출 또는 target 수신 경계 counter로 나눈 **추정치**다. 경계 counter는 polling 사이를 보간했다. Cold source는 in-flight reply가 남으므로 완전한 왕복당 비용이 아니다. CPU는 전체 CPU sample 대비 inclusive 비중이며 합산하지 않는다. Native Message 생성이 encoder 안으로 이동해 전/후 encoder 비중의 포함 범위도 달라졌다.

| 1K / 창 | source B/제출 전→후 | target B/수신 전→후 | source CPU% 전→후 | target CPU% 전→후 |
|---|---:|---:|---:|---:|
| request-backpressure cold | 620.2→565.9 | 860.2→표본 미관측 | 6.99→8.04 | 3.78→5.50 |
| request-backpressure steady | 1,299.2→961.1 | 911.4→574.8 | 8.71→7.60 | 15.38→16.76 |
| send-saturation cold | 156.3→122.9 | 0→0 | 5.73→4.73 | 1.53→1.79 |
| send-saturation steady | 121.9→124.5 | 0→0 | 2.91→2.81 | 1.38→1.83 |

L1의 envelope allocation/CPU 표본은 전후 0이다. 선택한 8개 run은 warmup/active 모두 errors=0, submitted=completed=received다(`summary.json` audit). 최종 active count는 L1 request 201,451, L2 request 161,410, L1 send 809,997, L2 send 748,637이다. 환경·pin 조건의 진단 count이며 throughput 개선률/7% 판정으로 사용하지 않는다.

JIT: 이전 canonical/decode 합계 2,360 bytecode → readHeader 968 bytecode. Request source의 old decode/readCanonical C2 완료는 start+5.61/+6.19초, 후보 readHeader는 +3.90초다. Target old +5.02/+6.26초 → 후보 +3.64초. **1–2초에는 두 버전 모두 해석/C1 구간이 남는다.** 후보 target readHeader C1은 +1.11초, source 첫 C1은 +2.13초다. 따라서 cold 비용 해소를 선언하지 않는다. 정확한 method/tier/시각은 각 `*-compilation.xml`, `summary.json`에 있다.

같은 JVM·binding에서 이전 source를 별도 classloader로 로드하고 80ms 창 10회씩 순서를 바꾼 보조 probe (`raw/paired-header-message.tsv`):

| 항목 | CPU ns/call 전→후 | allocated B/call 전→후 |
|---|---:|---:|
| request encode, prepared/native materialization 포함 | 378→505 | 496→96 |
| request parse | 667→739 | 512→424 |
| reply parse | 678→787 | 512→424 |
| correlation, UUID 생성 포함 | 52→30 | 168→136 |
| cached command control | 149→151 | 176→176 |

**encode CPU가 증가했다.** 낮은 할당만으로 성능 수정 완료로 판정하지 않는다. ByteBuffer/FFM view 후보도 CPU·할당 비교 후 제외했으며 raw는 보존했다. 추가 CPU 감소에 필요한 binding 후보 API는 `public MemorySegment mutableDataSegment()`다: 기존 Message-owned `cachedPayload`의 동일 writable segment를 추가 wrapper/copy 없이 반환하고, 유효기간은 Message close/transfer 전까지로 한다. 현재 public primitive write마다 반복되는 size/range 검사와 view 생성을 피할 수 있도록 검토해야 한다. 이 API는 구현하지 않았다.

## 검사와 남은 일

| 검사 | 결과 / 로그 |
|---|---|
| byte-identical / 값 | request/reply/command, deadline 유무, flow, metadata, escape, 1/64-byte correlation 새 matrix의 기존 코드 통과: `raw/header-bytes-before.xml`. 이후 Word 후보에서 native/heap·양 byte order·ASCII 경계 2,048조합·invalid JSON/Unicode 포함 51개 focused 통과: `raw/tests-word-first.log`, `raw/header-final.xml`. 최종 primitive writer 재검사는 대기 후 취소 |
| allocation fail-before/pass-after | native parse 520→432 B/header, escape parse 2,080→408, correlation 184→104. `raw/tests-before.log`, `raw/tests-correlation-before.log`, `raw/allocation-final.xml`. Native encode 기존 +416 B 실패 (`raw/tests-writer-before.log`); 최종 probe는 기본 Message 할당만 관측했으나 최종 JUnit 통과는 미확인 |
| package test | 이전 전체 4,146개/95 files, 실패·skip 0 (`raw/tests-packages-final.log`, `raw/packages-pre-word-summary.json`). 이후 전체도 통과 (`raw/tests-all-final.log`). 최종 `raw/tests-message-final.log`는 measure.gate 대기 후 취소했으며 완료 근거로 쓰지 않음 |
| build / format | 최종 composite build 성공 `raw/build-message.log`; `bash scripts/format/format.sh --check java` 성공 `raw/format-final.log`; diff check 성공 |

변경 파일: J, I, V 및 기존 Java test tree의 `ZLinkChannelEnvelopeGoldenTest.java`, `ZLinkEnvelopeCanonicalAllocationTest.java`.

남은 일: 지정 slot에서 최종 package test 완료, encode CPU 회귀 해소/후보 채택 판단, 감독자의 1초 warmup 6-cell 재측정. **merge-ready 결과가 아니다.**

로컬 WIP commit: **f3ccabce586154c8e728c0914a2cb3fb8e3cbb14**. Push하지 않았다.
