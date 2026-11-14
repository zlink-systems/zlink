# .NET L2 header 비용 수정

Branch `perf/4352-dotnet-l2-envelope`, baseline `6350275facc5`. Wire/API 동작을 유지한 production 수정이다. Raw는 모두 보존했다.

## serial 4K 판정

L1/L2 counted는 각각 43,976/33,983 제출=완료=수신, errors0이었다. 손실22.72%지만 L2 warm7,623.4→count6,796.4로10.85% 하락했고 반복 counted는 한 번뿐이다. 같은 warm끼리의 차이도13.34%여서 손실 전체를 잡음으로 지우지 않는다. 1K 3회는 G5가 L1 10.17%, L2 12.90%로5%를 넘어 기존6.7%와4K22.7%를 정밀한 크기 효과로 비교할 수 없다. [원시 결과](raw/historical-serial.json)

Payload 증가에 따른 추가 복사는 확인되지 않았다. L2−L1 allocation은 source1K476.95/4K486.70 B, target520.18/520.33 B/message로 거의 고정이다. Source body는 `69891f4446:Client/Program.cs:612-614`의 같은 `RawWire.Decode(span)`이고 target는 `ZLinkServer/Program.cs:364-369`에서 `CalculateSize()`만큼 새 reply body를 encode한다. Body 크기가 커질 때 양 단계에 공통으로 source 배열2개, target protobuf decode 배열1개의 비용이 커진다. Header-only 단계에4K body 복사를 더하는 코드는 없다. [원시 결과](raw/historical-serial.json), [호출 대조](call-shape.md)

**One-way 통과는 reply만의 문제라는 증거가 아니다.** D:188-225,377-410의 기존 cache 때문에 반복 command는 header encode plan·decoded header를 재사용하지만 request/reply는 매번 달라지는 correlation 때문에 전체 parse를 한다. Request target와 reply source 양쪽의 이름 변환·값 할당이 L2 비용에 들어간다.

## .NET / C++ 비용

`D`=`framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs`(최종), `C`=`framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp`; 다른 상대 경로도 각 언어 framework/src 기준이다. Before D의637은 같고1195/1363은 최종1203/1383으로 이동했다.

| 항목 | .NET | C++ / .NET 추가 비용 |
|---|---|---|
| request 생성 | `ZLinkClientCallCodec.cs:7-36`: Header record+hex string, value-type deadline | `runtime/messaging/client_call_codec.cpp:16-28,94-118`: value header, 짧은 ID는 std::string의 inline storage. .NET은 Header/ID가 managed heap |
| header 쓰기 | D:188,1203-1270: native Message에 prefix1회+동적 field 쓰기, size/write2pass | C:240-320,461-490,527-558: size/write2pass와 최종 native frame. .NET만의 full body buffer/copy는 없음 |
| deadline | 전 D:1229-1230,1363: 두 pass에서 각각 포맷. 최종 D:1210-1212,1250: 한 번 포맷한 stack span 공유 | `client_call_codec.cpp:34-61,110-115`는 생성 시1회 계산. .NET formatter2→1, 기존 fraction·offset 표현 유지 |
| request/reply 읽기 | D:377-410,414-582,637-677: 전13 field 이름을 UTF16로 복사, 긴 escaped 이름은 string 생성 | C:959-995,1077-1110: UTF8 literal/name 비교. 최종 .NET도 단일 UTF8 mapping이며 unknown Unicode 검사는 유지 |
| decoded 값 | D:486-506,578-582: request 값 string4+Header1, 실제 reply string3+Header1 | C:728-733,1301-1316의 view entry는 retained header의 string_view; owning entry는 문자열을 소유. `.hpp:41-150`의 값 구조체이며 .NET과 같은 개별 managed string/record가 없음 |
| reply 생성 | `Runtime/Channels/ZLinkChannelReplyWriter.cs:34-60`, D:177-181: prepared record clone+Message+TwoMessageParts | `runtime/channels/channel_reply_writer.cpp:12-21,41-45`: 값 header와 borrowed text, inline message parts. .NET record/parts wrapper 비용이 더 있음 |
| protocol 검사 | D:414-582,941 이후: JSON token/duplicate field 검사, protocol1회/입출력. dispatcher`:140`이 공통 decoder 호출 | C:168-227,1000-1068,1260: 구조/field/semantic 검사. 규범 검사는 제거하지 않음 |
| completion | `ZLinkClientCallCodec.cs:80-99,125-172`는 decode·kind/body 검사·dispose. L2 bench는 `Client/Program.cs:592-626`의 raw binding completion | CPP client call codec:69-91와 envelope decoder. Framework operation registry·deadline completion은 L5~L7이므로 L2 비용으로 세지 않음 |

## 수정과 소유

- D:637: 원래 UTF8 span을 길이별로 직접 비교한다. Escaped known 이름만 기존 `Utf8JsonReader.CopyString(byte span)`으로 unescape한다. Mapping은 한 곳이며 unknown 이름의 기존 Unicode 검사를 유지한다.
- D:1203: 기존 deadline formatter를 두 pass 밖으로 옮겨 한 번 계산한다. Writer 안의 stack buffer를 호출자로 옮겼으며 새 timestamp 알고리즘·flag·queue·lock·cache는 없다.
- 대안인 known-name14번 `ValueTextEquals` 순회는 escaped 이름을 반복 처리해 기각했다. [대안](raw/fieldprobe.json), [선택안](raw/fieldprobe-v3.json). 이 작은 probe의 시간은 처리량 판정에 쓰지 않는다.

소유 계층: Framework envelope codec. Spec: ClientServer §5.3, messaging-hot-path §3 E1 / §4.1 I3 / §4.3 W4. 교차언어: C++/Node/Java·Kotlin의 UTF8 이름 처리와 준비된 deadline을 대조했으며 공통 동작 변경 없음. 분류 **B(기존 내부 비용)**. [구현 전 결정 소유 표](raw/diagnosis-before-implementation.md)

규칙 수(판정 위치): field mapping1→1, 이름 Unicode/unescape1→1, protocol validation1→1, deadline formatting1→1, 합계 **4→4**. 기존 cache·gate·completion 소유자는 그대로다.

## profile · allocation

MemoryDiagnoser 방식의 동일 probe는4×5,000회 window 중 최솟값을 사용하며 준비용10,000회 호출은 제외했다. Native payload bytes와 binding 송수신은 아래 관리형 할당에 포함하지 않는다. [before](raw/alloc-before-exact.json), [after](raw/alloc-after-exact.json)

| 연산 | managed objects/message 전→후 | managed B/message 전→후 |
|---|---:|---:|
| source request 생성+header encode | Header+ID+Message =3→3 |216→216|
| target request header decode | 값 string4+Header =5→5 |328→328|
| target reply 생성+header encode | Header+Message+parts =3→3 |216→216|
| source reply header decode | 값 string3+Header =4→4 |280→280|
| fully escaped known 이름의 request decode | 위5+이름 string13 =18→5 |888→328|

일반 wire의 값 할당은 그대로이며 **escaped 이름의 임시 string13개·560B만 제거**했다. 일반 request/reply 한 왕복의 field-name UTF8→UTF16 복사는26→0, deadline 값 formatting2→1, protocol validation4→4다. Outbound prefix copy는 header당1→1, inbound whole-header byte copy0→0, L2가 더하는 body copy0→0이다. [코드·계수 근거](raw/diagnosis-before-implementation.md)

`69891f4446` ladder를 git archive로 추출하고 이 worktree의 production Release .NET을 연결했다. Binding/Core는 lad-dotnet과 같은 `/root/.nuget/packages/zlink/1.25.0`; native SHA256 `85f228638e89…`이다. Before binary와 source/hash는 [manifest](raw/before-built-manifest.json)에 고정했다. 다른 worktree/build output은 사용하지 않았다.

Profile은 discarded warm cell 후 warmup3s+active4s, `dotnet-trace`8 cpu-sampling+GC AllocationTick이다. Trace는 warmup부터 수집하되 표는 active UTC window만 남겼다. **정식 측정의10s/5s와 구분한다**. 양쪽 모든 thread는 CPU6–7,14–15의 부분집합·SCHED_OTHER이고, 아래6개 유효 run은 lifecycle10.81–15.33s, 오류0·제출=완료=수신을 확인했다. measure.lock은 취득하지 않았다. [모든 counter·정책·lifecycle·sample raw](raw/profile-audit-final.json)

| profile pattern | source L1 / L2 전→후: encode·reply decode samples | target L1 / L2 전→후: request decode·reply encode samples |
|---|---|---|
| request-serial4K | 0·0 /25·7→31·5 |0·0 /13·9→6·8|
| request-backpressure1K |0·0 /381·169→319·277 |0·0 /86·178→108·118|

L1에는 codec sample이 없고 L2에서 source 양 경로와 target 양 경로가 나타난다. **Serial 표본은 적으며 위 숫자를 CPU/throughput 개선률로 읽지 않는다.** 대기 thread를 포함한 전체 sample 대비 BP source codec 비중은1.48→2.54%, target1.13→1.06%였다. 이 짧은 실제 profile만으로 총 CPU 개선을 입증하지 않는다. [raw](raw/profile-audit-final.json)

보조 probe는 tiered compilation을 끄고 before→after→after→before 순서로 각 연산20만 회의 process CPU를 비교했다. Request decode 평균870→710ms, reply decode785→635ms였고, reply encode control은280→285ms였다. 일반 header decode의 반복 변환 감소를 뒷받침한다. Request encode435→415ms는 control 변동도 있어 정밀한 개선률로 쓰지 않는다. **Throughput/7% 예산 재판정은 감독자 측정으로 남긴다.** [호출 수·모든 개별 raw](raw/cpu-stable-summary.json)

## 검증

수정 전 golden request/reply bytes·decoded values2개, invalid Unicode2개 통과. Allocation 회귀2개는 escaped 이름 때문에368→928 B/header로 실패했다. [before](raw/regressions-before.log)

| 검증 | 최종 결과 |
|---|---|
| 관련 Framework tests |9개 class,279개 통과·실패/skip0. Golden bytes·값, escaped allocation2개, unknown Unicode/길이 경계, deadline fraction/offset, codec/flow/reply/error/handoff 포함. [raw](raw/focused-final.log)|
| allocation 양방향 회귀 |동일 assertion: 이전2개 실패→최종2개 통과. 일반 wire 할당은 유지하고 escaped 이름의560B를 제거했다. [before](raw/regressions-before.log), [after](raw/focused-final.log)|
| format |`scripts/format/format.sh --check dotnet`:1280개 통과. 최종 변경 후 재검사 진행 중. [첫 검사](raw/format-check.log), [최종 검사](raw/format-check-final.log)|
| Release build / freshness |production DLL과 source timestamp, client/target에 복사한 Release DLL SHA256 일치. [build](raw/build-after.log), 각 profile fingerprint.json|

변경 파일: D, `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/EnvelopeHeaderCacheHotPathTests.cs`.

리팩토링 점검: 중복 변환2건을 같은 수정에서 제거했다. Execution-gate 이중 판정·추가 동기화 발견0, 넘긴 항목0. 남은 test 실패0. 중간 신규 test의 C# 문법 오류는 최종 검사 전에 고쳤고 assertion은 완화하지 않았다. Profile 준비 실패·잘린 trace도 삭제하지 않고 판정에서 제외했다.

로컬 commit: 검증 마무리 후 기록. Push·GitHub·sub-agent·internal-docs 사용 없음.
