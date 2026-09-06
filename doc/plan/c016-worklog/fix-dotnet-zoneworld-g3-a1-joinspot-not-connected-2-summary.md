# .NET ZoneWorld G3 → A1 endpoint 등록 해제 수정

D-100의 B 승인에 따라 outbound intent 제거 시 native endpoint 등록을 해제하도록 수정했다.
`DisconnectTransport`가 다른 outbound intent의 존재를 판정하며, 같은 endpoint를 광고하는
inbound peer의 admission은 유지한다. 새 RID로 다시 outbound 연결한 뒤 원격 ActorJoin이
성공하는 TCP 회귀 테스트가 통과했다. ZoneWorld는 3회 모두 G3 뒤 A1과 전체 verdict
40/40이 통과했고, 전체 sample gate도 7/7 통과했다.

## 변경 범위와 근거

| 변경 파일:line | 결과 |
|---|---|
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:469` | `RemovePeerConnectionIfNotAdmitted`의 endpoint 중복 판정을 제거하고 transport 정리를 `RemovePeer(..., disconnect: true)`로 위임한다. Admitted/Draining intent의 제거 거부는 유지한다. |
| 같은 파일 `:11184` | `DisconnectTransport`의 endpoint replacement 후보를 다른 outbound intent로 한정한다. 마지막 outbound intent 제거 시 공개 `_socket.Disconnect(peer.Endpoint)`를 호출한다(`:11218`). Inbound 및 실제 outbound replacement가 있는 경우의 RID 정리는 기존 경로를 따른다. |
| `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MeshEndpointHandoverTests.cs:9` | inbound 전/후의 old intent 제거 두 순서 모두 native 등록 부재, inbound admission 보존, 다음 outbound의 Hello/Admit 및 원격 Join 성공을 검증한다. |
| 같은 테스트 파일 `:105` | 같은 endpoint의 다른 outbound intent가 남아 있으면 연결 등록을 보존하고 그 intent가 admission에 도달함을 검증한다. |
| `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj:145` | 새 회귀 파일을 명시적 compile 목록에 포함한다. |

두 곳에 각각 inbound 제외 조건을 추가하는 대안은 같은 판단을 중복 유지하므로 채택하지 않았다.
승인된 단일 소유자 안으로 판단을 모았으며, D-093의 logical intent 종결 알림과 D-097의
host admission seal·Draining·idempotent admission 경로는 수정하지 않았다.

- **소유 계층:** Framework mesh가 logical outbound intent의 수명과 공개 binding disconnect 호출을 소유한다. Core가 native endpoint 등록·자동 reconnect·physical pair 정리를 소유한다.
- **Spec 조항:** `03-spot-actor/03-mesh-node.ko.md:303–313`의 연결 의도·RID initiator, Core socket README `:869–875`의 endpoint 등록·reconnect intent 제거. D-093 보존 근거는 `04-actor-model.ko.md:668–679`와 `08-routing.ko.md:323–329`다.
- **교차언어 대조:** Java의 intent 제거는 endpoint disconnect를 호출한다. Node는 endpoint 전용 intent와 RID 지정 intent를 구분한다. 아래 호출 경로처럼 RID 지정 제거까지 동일하다고 주장하지 않는다.
- **변경 분류:** **B — 기존 Framework endpoint intent 소유권 결함**, D-100 승인 범위.

수정 전/후 규칙 수: **endpoint replacement 판정 위치 2 → 1**.
새 runtime 상태·timer·monitor·retry·budget·runner 대기 조건은 없다.

## 교차언어의 같은 전이

| 언어 | logical intent 제거 시 호출과 의미 |
|---|---|
| Java | `ZLinkJavaRawMeshNode.java:1032`의 `removePeerConnection`은 기본적으로 `disconnectEndpoint=true`를 넘기고 `:1069–1070`에서 `router.disconnect(removed.endpoint())`를 호출한다. Inbound의 advertised endpoint가 같다는 이유로 이 호출을 생략하지 않는다. 별도의 `replacePeerConnection`에서 `false`를 넘기는 것은 `:962–966`의 주석대로 선행 `requestPeerIntentClose`가 이미 endpoint를 종료했기 때문이다. |
| Node | `node-raw-mesh-backend.ts:526–538`의 `removePeerConnection`은 RID 없는 intent를 `disconnectPeerEndpoint`로, RID 있는 intent를 `disconnectPeer`로 보낸다. `raw-service-mesh-runtime.ts:348–358`은 공개 endpoint disconnect를 호출한다. RID 지정 경로 `:294–344`는 generation fence 뒤 `disconnectRid`를 우선하고 API 부재 시 endpoint disconnect를 호출한다. .NET과 같은 두 위치의 inbound endpoint 소유권 판단은 없다. |

Java/Node의 구현은 읽기만 했으며 이 작업에서 테스트하거나 변경하지 않았다. .NET에만 필요한
수정은 inbound의 advertised endpoint를 local outbound 등록의 소유권으로 잘못 사용한 조건을
제거하는 것이다. Node의 RID 지정 경로에 대한 동일 동작 보증은 이번 .NET 검증 범위 밖이다.

## 검증 환경

- Branch `main`, commit 없음. 이 작업은 Core·binding·다른 언어·sample·보호 문서를 수정하지 않았다.
  작업 시작 시 존재한 node 변경과 untracked 파일은 유지했다.
- rebuild13 Core SHA-256: `083588b48faaf5e1e640961802cfd94847396ea52770b10c2b94216258b79dce`.
  `core/build-dev/lib`, `.artifacts/wsl/install/zlink-core/0.17.0/lib` 및 사용한 NuGet cache의 native library가 일치한다.
- NuGet `Systems.Zlink.0.17.0.nupkg` SHA-256:
  `c8ae19c523a960e1458ed5ddc7fd9589a497ccd06ce5faee37662ffb64dcb9bf`.
- `TMPDIR=/dev/shm/zlink-tmp-dotnet`, `NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-c8ae19c523a960e1`,
  `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`, `UseSharedCompilation=false`,
  `MSBUILDDISABLENODEREUSE=1`, `DOTNET_CLI_TELEMETRY_OPTOUT=1`.
- 모든 dotnet gate는 `/tmp/zlink-dotnet-gate.lock`, sample은 추가로
  `/tmp/zlink-samples-gate.lock`을 사용한다. Lock은 `flock -n`으로 획득했다.
- Log·환경·gate script: `/tmp/zlink-d100/`. Sample은 기존 diagnostics `Normal`과
  spot-discovery trace, `ZLINK_SAMPLE_EVIDENCE_DIR`로 로그를 보존한다.

## 검증 결과

| 검증 | 결과 | 로그 |
|---|---|---|
| `MeshNodeShutdownSealTests` + `ZLinkMeshPeerAdmissionTests` focused | **19 passed, 0 failed** | `/tmp/zlink-d100/focused.log` |
| 새 endpoint handover 회귀 | **3 passed, 0 failed** | `/tmp/zlink-d100/handover-compiled.log` |
| Unit half `--filter 'FullyQualifiedName!~CanonicalActorJoinIngressReplyTests'`, 1회 | **1981 passed, 0 failed**, 2.56분 | `/tmp/zlink-d100/unit-half.log` |
| ZoneWorld 1회차 | **PASS, 40/40 verdicts, G3 뒤 A1 성공, exit 0** | `/tmp/zlink-d100/zoneworld-1.log` |
| ZoneWorld 2회차 | **PASS, 40/40 verdicts, G3 뒤 A1 성공, exit 0** | `/tmp/zlink-d100/zoneworld-2.log` |
| ZoneWorld 3회차 | **PASS, 40/40 verdicts, G3 뒤 A1 성공, exit 0** | `/tmp/zlink-d100/zoneworld-3.log` |
| `bash samples/run_samples.sh`, 1회 | **7/7 PASS, exit 0**. 포함된 ZoneWorld도 **40/40, G3 뒤 A1 성공** | `/tmp/zlink-d100/all-samples.log` |
| Diff whitespace·변경 범위 | **PASS** | `git diff --check` |

회귀 테스트는 실제 TCP socket을 사용한다. `IContext`의 공개 factory로 얻은 owner router를
테스트가 보관하고, intent 제거 직후 공개 `Disconnect(endpoint)`가 `NotFound`를 반환함을
검사한다. Binding 내부 접근·reflection·mock disconnect는 없다. 이어서 inbound peer의
intent·lifecycle·descriptor revision·LastChangedMs가 보존되고 Draining Update가 도달함을
확인한다. 다음 outbound의 원격 ActorJoin은 기존 2초 test budget으로 성공해야 한다.

ZoneWorld 각 실행의 `:138`은 replacement 시작, `:139`는 fresh A1 통과, `:141`은 G3 통과,
`:147`은 전체 완료다. Verdict 집계는 `/tmp/zlink-d100/zoneworld-verdicts.json`에 보존했다.
전체 samples 1회는 TicTacToe·Bingo·SupportChat·ShoppingMall·DeliveryDispatch·GameQuest·ZoneWorld를
완료했다. 포함된 ZoneWorld의 추가 실행은 `all-samples.log:307`의 A1, `:309`의 G3, `:315`의
전체 완료 및 `all-samples-zoneworld-verdicts.json`의 40/40으로 확인했다.

Sample의 actor 배치와 원격 Join 검증은 구분한다. G3 뒤 A1은 1·3회차에서는 replacement의
로컬 Join(`zoneworld-1-evidence/ZoneWorld/logs/zone-node-replacement.log:672–681`,
`zoneworld-3-evidence/ZoneWorld/logs/zone-node-replacement.log:686–692`), 2회차에서는
zone-node-1의 로컬 Join(`zoneworld-2-evidence/ZoneWorld/logs/zone-node-1.log:20435–20441`)이었다.
전체 samples에 포함된 추가 실행도 replacement의 로컬 Join이었다
(`all-samples-evidence/ZoneWorld/logs/zone-node-replacement.log:349–358`).
이 sample 경로를 원격 Join 증거로 사용하지 않는다. Outbound → inbound → outbound 뒤
replacement에서 owner로 보내는 원격 Join과 native 등록 해제는 위 TCP 회귀 테스트가
직접 검증한다. Sample의 배치·대기·assertion은 변경하지 않았다.

## BLOCKERS

**없음.** 요청된 구현과 검증을 완료했으며 남은 실패는 없다. rebuild13 Core·NuGet 해시는
모든 gate 종료 뒤에도 동일하다. Commit·package rebuild는 수행하지 않았다.
