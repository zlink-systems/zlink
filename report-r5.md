# #1083 .NET magic r5 — origin/main 병합 충돌 해소

기준: main의 공개 status 구조를 유지했다. main에서 삭제한 관찰 event DTO, 별도 sequence 상태, `LocationSnapshot` 경로는 되살리지 않았다. 브랜치의 상수화는 main 구조에서 계속 사용하는 값에만 적용했다.

| 충돌 파일 | 해소 |
|---|---|
| `ZLinkClientServerClientRuntime.cs` | main의 `ProcessLocalOwnerId`를 사용하고 중복 이름 `ProcessLocalOwner`를 버렸다. 계속 사용하는 `manual:`·`local:`·`auto:` 연결 키 접두사와 기존 service liveness 상수 참조는 유지했다. |
| `ZLinkClientServerRuntimeService.cs` | main의 공개 `ZLinkClientServerStatus` 비교·발행과 sequence 소유자를 유지하고, 삭제된 event DTO·`RetainedObservation`·별도 `Sequence`·`LocationSnapshot`을 되살리지 않았다. |
| `ZLinkMeshMonitoringModels.cs` | main의 `ZLinkLocationRuntimeSnapshot` 상태 상수 집합을 유지해 `HealthyState`를 포함하고 같은 값의 상수를 중복 정의하지 않았다. |
| `ZLinkRouteMeshRuntimeService.cs` | main의 `MeshPeerState` 기반 공개 status projection과 lane 내부 sequence 발행을 유지했다. 삭제된 `LocationSnapshot`·상태 event 경로와 사용되지 않는 admission 문자열 상수는 남기지 않았다. |
| `ZLinkFanoutMonitoringModels.cs` | main이 제거한 `ZLinkFanoutRuntimeEvent` 계층을 되살리지 않고 공개 status model만 유지했다. |
| `ZLinkFanoutRuntimeService.cs` | main의 `ZLinkLocationStoreHealth.ProjectSnapshot` 경로를 유지해 location 상태 결정을 한 소유자에 두었다. |

충돌 처리 후 `git diff origin/main`에서 위 여섯 파일의 추가 변경은 `ZLinkClientServerClientRuntime.cs`의 연결 키 접두사와 service liveness 상수 참조뿐이다. 공개 status 구조의 판정 위치 수는 main과 동일하며, 별도 event·sequence·location 판정 위치를 추가하지 않았다(추가 전/후 0→0).

## 검증

| 검사 | 로그 | 결과 |
|---|---|---|
| .NET 빌드 | `.artifacts/1083-magic-r5/build.log` | WSL `dotnet build src/Zlink.Framework/Zlink.Framework.csproj --nologo`: 경고 13건, 오류 0건, 통과. |
| ClientServer·RouteMesh·Fanout 관련 unit | `.artifacts/1083-magic-r5/related-unit.log` | `ClientServerChannelRuntimeTests`, `RouteMeshRuntimeServiceTests`, `FanoutAutomaticDiscoveryTests`, `CapacityMonitoringProjectionTests` 필터: 129/129 통과. |
| `scripts/format/format.sh --check dotnet` | `.artifacts/1083-magic-r5/format.log`, `format-lf.log` | 첫 검사는 Windows→WSL 복사에서 48개 `.cs` 파일의 CRLF가 보존되어 9개 파일의 줄바꿈 차이만 보고했다(`format.log`: 1,152개 검사, 9개 사유 모두 line endings). Windows 작업 파일은 변경하지 않고 WSL 사본만 LF로 정규화한 뒤 동일 검사를 재실행해 1,152개 검사, exit 0으로 통과했다. |

리팩토링 점검: 성능 0건·POSDDD 0건·불필요 코드 7건 발견, 사용되지 않는 admission 문자열 상수 7개를 제거하고 main이 삭제한 event·sequence·location 코드를 되살리지 않았으며 넘긴 항목은 0건이다.
