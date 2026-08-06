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
