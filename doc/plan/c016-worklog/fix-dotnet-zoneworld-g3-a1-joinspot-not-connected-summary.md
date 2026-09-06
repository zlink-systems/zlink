# .NET ZoneWorld G3 → A1 JoinSpot 연결 실패 진단

현재 단계는 **진단 완료, B 분류 구현 승인 대기**다. Runtime 수정과 수정 후 gate는 아직 수행하지
않았다. 기존 코드와 rebuild13으로 별도 재현에서 같은 Join completion `109`를 확인했다.

원인은 sender의 조기 종료가 아니라 **앞선 교체에서 제거한 outbound intent의 native endpoint
연결 등록을 남긴 것**이다. 같은 endpoint를 광고하는 inbound peer를 outbound 연결 등록의
새 소유자로 취급하여 disconnect를 생략한다. 다음 교체에서 outbound로 돌아오면 새 RID로
Hello를 보내도 상대에서 수신되지 않는다. 이 때문에 replacement의 Join은 한 번도 transport
admission되지 못하고 기존 15초 deadline을 소진한다.

## 환경과 범위

- Branch `main`; commit, package rebuild 없음.
- Core library SHA-256: `083588b48faaf5e1e640961802cfd94847396ea52770b10c2b94216258b79dce`.
  `core/build-dev/lib/libzlink.so.0.17.0`와 `.artifacts/wsl/install/zlink-core/0.17.0/lib/`가 일치한다.
  HEAD의 후속 drain 수정이 들어간 패키지로 바꾸지 않았다.
- `Systems.Zlink.0.17.0.nupkg` SHA-256:
  `c8ae19c523a960e1458ed5ddc7fd9589a497ccd06ce5faee37662ffb64dcb9bf`.
- `TMPDIR=/dev/shm/zlink-tmp-dotnet`,
  `NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-c8ae19c523a960e1`,
  `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`,
  `UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`, `DOTNET_CLI_TELEMETRY_OPTOUT=1`.
  두 test 실행 모두 `/tmp/zlink-dotnet-gate.lock` 사용.
- 저장소 변경은 이 보고서 하나다. 기존 사용자 변경 및 다른 작업의 node 변경은 건드리지 않았다.
  별도 재현 source·MSBuild import·실행 script·log는 `/tmp/zlink-g3-a1-diagnosis/`에 있다.

## 보존 로그의 순서

아래 `F/`는
`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/dotnet-zw-a1-fail-run/logs/`다.

| 순서 | 관찰 및 근거 |
|---|---|
| 기존 zone 소유자 | `F/zone-node-1.log:19`, `:296`: `zone-nw`는 zone-node-1, RID `zn-81fe868a-b3a0-4403-80f7-c408ac736a44`, lifecycle `1485195403363677577`, owner `b9e812b019784050aa063fdca123e0be`, lease 3. |
| 이전 outbound | `F/zone-node-1.log:15564–15577`: endpoint `tcp://127.0.0.1:22942`, RID `zn-ac109ad0-ec4a-4a64-8eb5-510eb3d82870`, intent 6으로 connect 후 Admit 성공. |
| 방향 전환 | `:16447–16461`: 같은 endpoint의 새 RID `zn-7cab6850-4555-4699-99bd-4dbf9c497eaa`가 Hello를 보내고 inbound intent 7로 admission됨. RID 순서가 `7cab < 81fe < ac10`이므로 정상적인 initiator 전환이다. |
| 원인 전이 | `:16470`: 이전 outbound intent 6은 `state=Connecting disconnect=False`로 제거된다. `:16471–16472`는 release/handover 성공을 보고하지만 native endpoint disconnect는 없다. |
| G3 stop | `:18735–18738`: `7cab…`의 Draining Update. `:18761`: inbound intent 7이 `disconnect=False`로 제거된다. |
| G3 replacement | `:18835–18839`: 새 RID `zn-db52a991-c5f8-499d-ba4a-fc506f721174`에 outbound intent 8 생성. 이후 `:20949`까지 Hello 시도 42회. `:21007`의 정리 직전까지 intent 8은 Connecting이다. |
| replacement 관점 | `F/zone-node-replacement.log:10–14`: owner `81fe…`가 desired `await`로 존재한다. `:23`, `:74`는 ops와 gateway admission. owner와의 admission 기록은 전체 로그에 없다. |
| A1 생성·Join | `:574–589`: actor `a1-81354a`가 replacement에 생성됨. `:607` 08:19:35.943에 Join 예약. `:609–610`은 `zone-nw`의 Ready authority와 동일 owner/lease 3을 resolve한다. |
| deadline 직전 | `:1621–1623`: owner `81fe…`는 여전히 live/desired `await`; intent 제거가 관측되지 않는다. |
| 종료 | `:1624` operation `066b5ad8f35fc4b3000000000000001c` ActorJoin completion. `:1625–1636` Unavailable/109, 08:19:50.957. `:1639` 08:19:50.968에 오류를 담은 JoinWorldRes 송신. 약 15초이며 ZoneNode `Program.cs:55`의 기존 timeout과 일치한다. |

`ops.log:2656`은 새 RID를 이미 관측했다. `client.log:45`와 gate log `:137–139`의 A1/G3 실패는
RID 게시 실패나 STREAM 응답 순서 역전의 증거가 아니다.

### D-093 및 admission 판정

- Framework operation 등록은 성공했다. `TryRequestCanonicalActorJoin`은
  `Runtime/Service/ZLinkManagedMeshNode.cs:2310–2316`에서 operation을 등록하고 `:2348`에서
  durable sender를 시작한다. 이것은 Core REQUEST admission과 다른 단계다.
- `:9262–9274`는 매 attempt마다 `RequireDirectPeer`를 먼저 호출한다. 해당 peer가 한 번도
  admitted되지 않았으므로 binding request에 도달하지 못한다. 원 로그에는 개별 replay attempt
  수가 없으므로 횟수를 주장하지 않는다.
- `Runtime/Messaging/ZLinkDurableRequest.cs:30–44`, `:69–75`는 submit 실패를 재전송하고,
  한 번도 admission되지 않은 operation의 deadline 소진을 Unavailable로 판정한다.
- `ZLinkManagedMeshNode.cs:9281–9286`은 이를 `RequestResult.NotConnected`로 넘긴다.
  `ZLinkRequestFailureMapper.cs:97–101`, `:238–240`이 **새로 만든** inner
  `ZlinkRequestException(109)`을 붙인다. Inner exception의 형식만으로 실제 native request가
  admission된 뒤 109 completion을 받았다고 판단할 수 없다.
- 따라서 이번 종료는 D-093 rule 1의 logical intent 제거에 따른 조기 종결이 아니다.
  Actor model의 **never admitted → deadline exhaustion → Unavailable**다. D-093이 이 별도
  deadline 규칙을 없애지는 않는다. 15초 뒤에도 연결되지 않는 원인을 수정해야 한다.

## 통과 경로와 교차언어 대조

같은 실패 run의 첫 A1 actor `a1-b7834a`는 `F/zone-node-1.log:1098–1110`에서 zone 소유 노드에
생성됐다. `:1125–1134`는 08:17:00.538 Join → .540 local admission/reply → .550 JoinWorldRes를
보인다. replacement에서 owner로 보내는 remote Join이 아니었다.

같은 rebuild13의 앞선 통과 run은 `/dev/shm/zlink-tmp-dotnet/tmp.5YXpFMP4uT/logs/`이며,
비교 로그를 `/tmp/zlink-g3-a1-diagnosis/passing-run/`에 복사했다.

- `zone-node-replacement.log:15–28`: replacement `zn-0d3497e2…`가 owner `zn-d0459218…`로
  직접 dial하고 Admit까지 수신한다. 이번 실패의 반대 방향이다.
- `zone-node-1.log:19629`, `:19662–19671`: G3에서 다시 실행한 A1 actor `a1-7eefb6`도
  **owner에 생성돼 local Join**을 수행한다. `gate13-dotnet-samples.log:352–360`은 A1/G3와
  전체 verdict 통과다. 통과한 A1이 replacement → owner remote Join을 검증한 것은 아니다.
- 이전 Core의 `gate12-dotnet-zoneworld-2.log:137–145` 역시 A1/G3와 전체 verdict 통과다.
  이것만으로 이번 방향 전환이 검증됐다고 주장하지 않는다.

.NET runner `samples/ZoneWorld/run_sample.sh:980–994`는 topology ready, ops의 새 RID 관측 뒤
A1을 실행한다. Java `samples/java/ZoneWorld/run_sample.sh:327–337`도 topology ready 뒤
`ZW-G3-fresh`를 실행하되 새 RID에 배치됐다는 proof를 검사한다. Node
`samples/ZoneWorld/Runner/sample-runner.mjs:303–326`도 G3 fresh actor proof를 검사한다.
Runner 대기를 늘려도 이미 남은 endpoint 등록은 해제되지 않는다.

Java `ZLinkJavaRawMeshNode.java:1032–1091`은 outbound intent 제거를 `router.disconnect(endpoint)`와
연결한다. Node `raw-service-mesh-runtime.ts:348–358`의 endpoint 제거 경로도 공개 disconnect를
호출한다. Node의 RID 제거 경로(`:294–344`)는 별도 generation fence가 있어 Java와 완전히 같은
구조라고 주장하지 않는다. .NET 특이점은 inbound의 advertised endpoint가 같다는 이유로
outbound 등록 제거를 생략하는 조건이다.

## 원인과 검토 가능한 수정안

`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs` 기준:

1. `:477–488` `RemovePeerConnectionIfNotAdmitted`는 방향을 구분하지 않고 같은 endpoint의
   다른 peer가 있으면 `RemovePeer(..., disconnect:false)`를 호출한다. Inbound peer는 그
   endpoint로의 local connect 등록을 소유하지 않는데도, outbound 등록을 유지하는 근거가 된다.
2. `:11204–11226` `DisconnectTransport`에도 같은 endpoint 조건이 중복돼 있으며, 역시
   inbound peer까지 replacement로 취급한다. 위 조건만 지우면 이쪽에서 RID disconnect만 하고
   outbound endpoint intent는 계속 남길 수 있다.
3. `:10629–10635`의 다음 `SetConnectRoutingId(newRid)` + `Connect(endpoint)`는 이전 local
   연결 등록이 정상적으로 해제됐다는 전제 위에서 동작한다. Framework가 이전 등록을 남긴 것이
   문제이며, Core의 자동 reconnect를 재구현할 이유는 없다.

| 대안 | 판단 |
|---|---|
| runner readiness 조건/대기 또는 sender retry 추가 | 기각. 영구적으로 남긴 endpoint 등록의 원인을 해결하지 않는다. |
| 두 곳에 inbound 제외 조건을 각각 추가 | 기각. 같은 endpoint 소유권 판단을 두 곳에 계속 둔다. |
| `RemovePeerConnectionIfNotAdmitted`의 중복 판단을 제거하고 기존 `DisconnectTransport`가 실제 outbound replacement만 판별 | 제안. logical outbound intent 제거 시 native endpoint 등록 제거를 한 곳에서 결정한다. |

제안한 변경은 아직 적용하지 않았다. 검증 시에는 inbound replacement의 정상 admission을
보존하면서 outbound → inbound → outbound 이후의 remote Join이 성공해야 한다. 실제 outbound
replacement를 제거하지 않는 기존 계약도 함께 확인해야 한다.

**소유 계층:** Framework mesh의 logical outbound intent 수명과 공개 binding disconnect 호출;
Core는 endpoint 연결 등록·자동 reconnect·물리 pair 정리를 소유한다.

**Spec 조항:** `03-spot-actor/03-mesh-node.ko.md:303–313`의 연결 의도·RID initiator,
`04-actor-model.ko.md:668–679`의 durable replay/소진 원인,
`08-routing.ko.md:323–329`의 owner 종료;
Core socket README `:869–875`의 endpoint disconnect에 따른 local 등록·reconnect intent 제거.

**교차언어 대조:** Java/Node의 endpoint 제거는 공개 disconnect를 호출한다. .NET은 같은 endpoint의
inbound peer 때문에 outbound 등록 제거를 생략한다. Runner의 추가 대기는 수정 대상이 아니다.

**변경 분류:** B — 기존 Framework endpoint intent 소유권 결함. 감독 승인 요청 상태이며 구현 전이다.

수정 전/후 규칙 수: 실제 변경 **2 → 2(미적용)**. 제안은 endpoint replacement 판정 위치
**2 → 1**, 새 상태·timer·monitor·retry budget **0**이다.

## 재현과 검증 결과

별도 재현은 repository source를 변경하지 않고 기존 test assembly에
`/tmp/zlink-g3-a1-diagnosis/diagnostic.targets`로 diagnostic source를 포함했다.
Binding private API나 reflection을 사용하지 않는다. Core·binding 수정은 없다.

`ReplacementEndpointDiagnostic.cs`는 실제 TCP mesh에서 다음을 실행한다:
owner `mesh-mid` → `mesh-z-old` outbound admission, old 종료, `mesh-a-middle` inbound admission,
old intent 제거, middle 정상 Draining/종료, `mesh-z-replacement`로 다시 outbound 연결,
replacement에서 owner의 `zone-nw`로 canonical ActorJoin. 기존 sender의 2초 test budget을 사용한다.
수신측 Join은 도달하면 Accepted를 반환하고 source completion은 Ok여야 한다.

추가 대조에서는 runtime을 그대로 두고 old intent 제거 시점만 비교했다. Inbound가 존재한 뒤
제거하면 109로 실패하고, inbound가 생기기 전에 제거하여 기존 코드가 endpoint disconnect를
실행하게 하면 Join이 성공한다. 이는 원인 분리를 위한 진단 변수이며, sample의 순서를 바꾸는
수정안이 아니다. 두 경우 모두 성공해야 한다는 assertion은 유지했다.

| 검증 | 결과 | 로그 |
|---|---|---|
| 기존 DurableSender/DurableRequest/RemoteJoinerCanonicalReply/DeferredActorJoinDurability focused | **34 passed, 0 failed**, 21초 | `/tmp/zlink-g3-a1-diagnosis/focused.log` |
| 새 TCP 방향 전환 diagnostic, 기존 runtime | **재현 성공: 기대 0, 실제 109**. Test 1 failed, 총 3.34초 | `/tmp/zlink-g3-a1-diagnosis/repro.log:23–48` |
| 같은 diagnostic의 intent 제거 시점 대조 | inbound 전 제거 **PASS**, inbound 후 제거 **109 FAIL**; 1 passed / 1 failed, 4.16초 | `/tmp/zlink-g3-a1-diagnosis/repro-control.log` |
| `git diff --check` 및 보고서 trailing whitespace | PASS | 작업공간 실행 |
| 수정 후 새 회귀 및 관련 mesh tests | 미실행: 수정 승인 대기 | — |
| 수정 후 ZoneWorld ×3, G3 뒤 A1 및 전체 verdict | 미실행: 수정 승인 대기 | — |
| 수정 후 `bash samples/run_samples.sh` 1회 | 미실행: 수정 승인 대기 | — |
| 수정 후 unit half 1회 | 미실행: 수정 승인 대기 | — |

## BLOCKERS

- `AGENTS.md` §3: “감독이 A 또는 B로 승인한 뒤에만 2단계 구현을 시작한다.” 이번 진단과
  B 분류 수정안을 보고하고 승인 질문을 제출했으나 답은 아직 없다. Runtime 코드와 repository
  회귀 테스트의 변경, 수정 후 최종 gate는 이 승인 뒤 진행한다.
- 수정 완료 또는 ZoneWorld 해결을 주장하지 않는다. 새 diagnostic의 109는 원인 재현 결과이며
  아직 해결되지 않은 실패다.
- G3의 .NET A1 재실행은 새 RID에 실제 배치됐다는 proof를 검사하지 않는다. 통과 run에서
  owner-local Join이었다는 관측을 기록하며, 이 별도 runner 검증 범위를 이번 원인 수정과
  혼동하지 않는다.
