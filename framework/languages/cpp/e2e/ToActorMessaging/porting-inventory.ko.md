# ToActorMessaging C++ porting inventory

| 항목 | C++ 위치 | 상태 |
|------|----------|------|
| shared messages | `Shared/messages.hpp` | implemented |
| actor role | `Server/Actor/main.cpp` | implemented |
| caller role public actor client call site | `Server/Caller/main.cpp` | implemented |
| client TA-A1..TA-B3 scenario list | `Client/main.cpp` | implemented |
| runner | `run_e2e.sh` | implemented; 필요한 actor/caller/client target을 빌드한 뒤 Redis, 서버 readiness, client 검증을 실행한다. |

## 최신 검증

- `E2E_START_ORDER=reverse ZLINK_CPP_E2E_BUILD_DIR=/home/hep7/project/kairos/zlink/framework/languages/cpp/build-redis-vcpkg CMAKE_BUILD_PARALLEL_LEVEL=1 nice -n 10 timeout 240s framework/languages/cpp/e2e/ToActorMessaging/run_e2e.sh`
  - 결과: `to-actor-messaging e2e result=passed`
  - 로그: `framework/languages/cpp/e2e/ToActorMessaging/logs/20260707-182812-3053142`
  - 의미: actor/caller 서버를 모두 시작한 뒤 readiness를 기다리므로 서버 구동 순서에 의존하지 않는다.
