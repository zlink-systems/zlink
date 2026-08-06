# Kotlin StoreFailure E2E feature map

Config 6의 Kotlin fixture 구현은 기존 `DiscoveryRegistryHa` process를 사용한다. 이 canonical
entry point는 aggregate runner가 공통 suite를 누락하지 않도록 selector를 소유한다.

`SF-A1`부터 `SF-E1`까지는 기존 process·Redis·public runtime query evidence를 실행한다. `SF-B3`,
`SF-C3`부터 `SF-C5`, `SF-F1`부터 `SF-F11`, `SF-G1`부터 `SF-G3`은 현재 Kotlin public fixture가
제공하지 않는 relocation store, object page query, cross-language process, actor/user-spot factory
및 capacity control이 필요하므로 runner가 성공으로 분류하지 않고 명시적인 exit 3 blocker로
기록한다. marker-only 성공이나 private API 우회는 사용하지 않는다.

실행 구현은 `../DiscoveryRegistryHa/run_e2e.sh`와 그 feature map이 소유하고, 이 문서는 canonical
suite의 선택자·public contract 경계를 소유한다.

`SF-B2`는 공통 시나리오의 순서를 맞춰 runner를 수정했다. Provider A만 ready인 상태에서 Store를
failure grace보다 오래 중단한 뒤 B를 시작하고, 장애 중에는 A request만 성공하며 B를 ready target으로
추가하지 않는지 확인한다. Store 복구 뒤 장애 중 시작한 B가 startup retry를 통해 current target set에
들어가는지 확인한다. 초기 owner claim retry와 descriptor generation recovery는 Framework runtime에
반영했다. stale owner의 MeshNode descriptor를 이전 owner token으로 제거한 뒤 새 lease
generation으로 재게시하는 recovery 경로를 추가했고, `0.10.0` local package로 재배포한 뒤
SF-B2가 통과했다. 장애 중에는 `api-a`만 요청 대상이고 `api-b`는 제외되며, 복구 후에는
`api-a`와 `api-b`가 `READY` target으로 수렴한다.

`SF-D2`도 공통 절차와 대조해 실행했다. A와 B가 ready인 상태에서 긴 Store 장애 중 B를 강제 종료한
뒤 복구했을 때, A의 기존 connection과 lease 재게시가 유지되어야 한다. 실제 실행에서는 장애 전
`api-a`와 `api-b`의 peer-ready evidence가 있었지만 복구 후 consumer topology query가 `api-a`의
`READY` row도 찾지 못해 실패했다. 이후 stale descriptor recovery를 수정하고 동일
`0.10.0` package로 재실행한 결과 SF-D2도 A traffic 유지, A 재게시, 종료된 B 제외 조건을
통과했다.
