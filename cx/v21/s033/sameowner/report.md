# #1639 same owner 재현

## 결과

- 공개 C++ Client/Channel API 재현 20회: A가 latch에서 동기 차단된 동안 같은 Channel의 B 진입 `0/20`, 다른 Channel B 진입 `20/20`이었다([실행 로그 1행](./repro.log:1), [재현 코드](</root/worktree/zlink-v21-s033/sameowner/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_client_server_runtime.cpp:363>)).
- 같은 owner B가 시작하지 않은 결과는 §2의 gate-less Node/Channel 독립 실행 계약과 어긋나므로 **분류 B(기존 계약 결함)**다([spec](</root/worktree/zlink-v21-s033/sameowner/framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:97>)).
- 다른 owner 대조는 통과했다. Actor gate 대조는 실행 전에 앱 readiness를 얻지 못해 판정하지 못했다([실행 로그 2행](./repro.log:2), [Actor 대조](</root/worktree/zlink-v21-s033/sameowner/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_client_server_runtime.cpp:435>)).
- .NET/Java는 실행하지 않고 소스만 확인했다. .NET은 batch 시점 record 수를 고정하고 release 시 남은 record를 다시 ready로 알린다([mailbox](</root/worktree/zlink-v21-s033/sameowner/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshOwnedMailbox.cs:104>), [claim/release 연결](</root/worktree/zlink-v21-s033/sameowner/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:2833>), [batch handler 결과 대기](</root/worktree/zlink-v21-s033/sameowner/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:247>)). Java는 claim된 owner의 재claim을 건너뛰고 이전 claim 종료 때만 ready queue에 되돌린다([mailbox](</root/worktree/zlink-v21-s033/sameowner/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceMailbox.java:56>), [claim 종료](</root/worktree/zlink-v21-s033/sameowner/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkHandlerDispatch.java:63>)). 두 언어 모두 같은 시나리오의 실행 결과가 아닌 소스상 판정이다.

## 결정 소유 표

| 결정 | 근거 및 현재 판정 | 수정 시 단일 소유자 |
|---|---|---|
| 같은 Node/Channel owner의 다음 claim 허용 여부 | C++ `try_claim_owner`가 `active_messages != 0`이면 거부하며, `claim_owner_locked`도 같은 조건을 재검사한다([service_mailbox.cpp](</root/worktree/zlink-v21-s033/sameowner/framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:199>)). | Framework `service_mailbox_t`의 owner claim 정책. 변경하지 않았다. |

## 빌드와 산출물

- 요청한 vcpkg preset 구성은 성공했고 지정 타깃 `test_cpp_framework_client_server_runtime` 빌드도 한 번 성공했다. 이후 Actor 대조용 location polling/lease 설정과 실패 진단을 보완했지만, 최종 incremental build는 다른 작업의 전역 `measure.lock` 및 compile slot 대기 때문에 완료하지 못했다.
- 실행한 특수 재현은 Channel 조건을 마친 뒤 Actor 앱 readiness assertion에서 종료됐다([repro.log 1–2행](./repro.log:1)). 따라서 Actor gate 20회 판정은 없다.
- 제품 runtime 수정은 없다. 테스트 변경은 [repro.diff](./repro.diff)에 보존했다. worktree test source는 [test_cpp_framework_client_server_runtime.cpp](</root/worktree/zlink-v21-s033/sameowner/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_client_server_runtime.cpp:234>)다.
