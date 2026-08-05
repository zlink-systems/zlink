# .NET Config 14 Instance Spot E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-14-instance-spot.ko.md`

이 파일은 Config 14의 전체 시나리오 분모를 고정하는 feature map이다. 현재
`SpotService` process fixture는 Track A의 세 시나리오와 idle eviction 재활성화 시나리오를
실행한다. 이 표에서
`검증 완료`는 해당 시나리오를 새 process로 실행하고 로그와 assertion을 확인했다는
뜻이며, 나머지 항목의 `미구현` 상태를 줄여서 해석하지 않는다.

| 시나리오 | 상태 | 근거 또는 blocker |
|----------|------|-------------------|
| IS-E2E-01 | 검증 완료 | `framework/languages/dotnet/e2e/SpotService/logs/20260804-145911-582473/`에 cold request와 `instance-initialize` 1회가 기록됐다. Client assertion은 응답의 Spot ID·operation ID도 확인했다. |
| IS-E2E-02 | 검증 완료 | `framework/languages/dotnet/e2e/SpotService/logs/20260804-150030-587658/`에 cold send와 `instance-initialize` 1회가 기록됐다. Client assertion은 send 수락 결과를 확인했다. |
| IS-E2E-03 | 검증 완료 | `framework/languages/dotnet/e2e/SpotService/logs/20260804-150104-591087/`에 동일 Spot의 `instance-initialize` 1회와 서로 다른 두 operation의 `instance-request`가 기록됐다. Client assertion은 두 응답이 같은 owner를 가리키는지 확인했다. |
| IS-E2E-04 | 미구현 | 서로 다른 Instance Spot의 execution queue 독립성을 검증하는 runner가 없다. |
| IS-E2E-05 | 미구현 | Ready owner crash 뒤 자동 takeover가 없는지 검증하는 runner가 없다. |
| IS-E2E-06 | 미구현 | creating owner crash의 recovery 경계를 검증하는 runner가 없다. |
| IS-E2E-07 | 미구현 | 정상 relocation 뒤 identity와 state 보존을 검증하는 runner가 없다. |
| IS-E2E-08 | 검증 완료 | `framework/languages/dotnet/e2e/SpotService/logs/20260804-165721-1053531/`에 `IdleEvicted` 종료 뒤 같은 Spot ID의 두 번째 cold request와 `instance-initialize` 2회가 기록됐다. `instance-idle` process runner가 두 request의 성공과 새 instance 생성을 확인했다. |
| IS-E2E-09 | 미구현 | owner crash 뒤 concurrent request의 bounded failure를 검증하는 runner가 없다. |
| IS-E2E-10 | 미구현 | stale owner resume 뒤 자동 owner 생성이 없는지 검증하는 runner가 없다. |
| IS-E2E-11 | 미구현 | confirmed not admitted terminal을 검증하는 runner가 없다. |
| IS-E2E-12 | 미구현 | ambiguous result 처리와 후속 상태를 검증하는 runner가 없다. |
| IS-E2E-13 | 미구현 | accepted send 뒤 failure 경계를 검증하는 runner가 없다. |
| IS-E2E-14 | 미구현 | Store outage 중 activation 결과를 검증하는 runner가 없다. |
| IS-E2E-15 | 미구현 | kind·type atomic conflict를 검증하는 runner가 없다. |
| IS-E2E-16 | 미구현 | eligible node가 없을 때의 terminal을 검증하는 runner가 없다. |
| IS-E2E-17 | 미구현 | activation backpressure를 검증하는 runner가 없다. |
| IS-E2E-18 | 미구현 | cross-language Instance Spot 의미를 검증하는 runner가 없다. |
| IS-E2E-19 | 미구현 | first activation과 후속 message ordering을 검증하는 runner가 없다. |
| IS-E2E-20 | 미구현 | closing owner crash 경계를 검증하는 runner가 없다. |
| IS-E2E-21 | 미구현 | multi-Mesh initial placement를 검증하는 runner가 없다. |
| IS-E2E-22 | 미구현 | owner deadline의 monotonic semantics를 검증하는 runner가 없다. |
| IS-E2E-23 | 미구현 | Instance Spot handler capability를 검증하는 runner가 없다. |
| IS-E2E-24 | 미구현 | 늦은 Store response가 activation 결과를 오염시키지 않는지 검증하는 runner가 없다. |
| IS-E2E-25 | 미구현 | activation completion failure와 cleanup을 검증하는 runner가 없다. |
| IS-E2E-26 | 미구현 | concurrent claim의 단일 owner 결과를 검증하는 runner가 없다. |
| IS-E2E-27 | 미구현 | 서로 다른 activation deadline의 격리를 검증하는 runner가 없다. |
| IS-E2E-28 | 미구현 | close와 admission의 경쟁 결과를 검증하는 runner가 없다. |
| IS-E2E-29 | 미구현 | cross-Mesh in-flight relocation을 검증하는 runner가 없다. |
| IS-E2E-30 | 미구현 | multi-Mesh concurrent relocation을 검증하는 runner가 없다. |
| IS-E2E-31 | 미구현 | remote selection loser의 reservation 정리를 검증하는 runner가 없다. |
| IS-E2E-32 | 미구현 | activation crash boundary를 검증하는 runner가 없다. |
| IS-E2E-33 | 미구현 | cold activation failure release를 검증하는 runner가 없다. |
| IS-E2E-34 | 미구현 | unpublished activation cleanup을 검증하는 runner가 없다. |
| IS-E2E-35 | 미구현 | Ready owner crash 뒤 queue 자동 복구 금지를 검증하는 runner가 없다. |
| IS-E2E-36 | 미구현 | first handler terminal recovery를 검증하는 runner가 없다. |

Track A와 IS-E2E-08의 실행 진입점은 `framework/languages/dotnet/e2e/InstanceSpot/run_e2e.sh`에
등록되어 있다. `run_e2e_all.sh`에서 Config 14 전체를 성공으로 보고하지 않는 이유는
나머지 32개 시나리오에 아직 독립적인 process 검증 경로가 없기 때문이다.
