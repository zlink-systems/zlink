# same-owner 동기 handler 차단 재현 — #1639

## 결과

- 기준 worktree를 `76a4d78506` detached로 만들었다. #3891은 `bf24a5a02c`이며, commit message와 diff는 gate 없는 Node/ChannelName owner에서 여러 worker의 record 처리를 허용하도록 S02/S04/S08을 바꾼 것으로 확인했다.
- C++ 정적 경로는 **B(기존 계약 결함)** 이다. `try_claim_owner`와 `claim_owner_locked`가 `active_messages != 0`이면 새 claim을 거부한다 (`framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:212-217,235-237`). claim은 선택된 각 record를 active로 올리고 (`:263-277`), 마지막 settlement에서야 claim을 끝낸다 (`:432-452`). handler A 실행 중에는 그 claim이 active이므로 동일 owner의 B는 새 claim을 얻지 못한다. 이는 S02 §2의 record별 독립 turn과 다른 application worker의 pipeline 진입 허용 (`framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:97-105`)과 충돌한다.
- 이는 실행 로그에 의한 재현 결과가 아니다. 공개 API 재현, 서로 다른 owner 대조, gated owner 대조를 작성·실행하지 못했다. 따라서 20회 반복 결과도 없다. `repro.diff`는 생성하지 않았다.
- 빌드 설정은 `opentelemetry-cpp` CMake package가 없어 실패했다. 지정한 vcpkg 경로 `/root/.cache/zlink/vcpkg`에는 `installed/`가 없었다. 빌드 slot compile, `-j3`, 25 GB cap 설정으로 빌드를 시작하지 못했다.
- .NET/Java의 같은 public API 시나리오는 실행하지 않았다. 읽기만 한 .NET 경로는 batch의 각 invocation을 dispatch하고 미완료 task를 수집한다 (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:220-254`). Java `ZLinkHandlerDispatch`는 serial queue에 operation을 넣는다 (`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkHandlerDispatch.java:16-45`); 이 정적 확인만으로 두 언어의 blocked-handler 동작을 판정하지 않는다.

## 결정 소유 표

| 결정 | 현재 소유 조항 | 현재 판정 위치 | 단일 소유자 방향 |
|---|---|---|---|
| Node/ChannelName의 record 동시 실행 허용 | S02 §2 (`02-handler-turn-and-execution-gate.ko.md:97-105`) | C++ mailbox가 owner 전체의 active claim을 배타화 (`service_mailbox.cpp:212-217,235-237`) | owner queue의 record take/claim 권한을 소유하는 mailbox에서 record별 실행 허용을 판정. Scheduler는 ready/wake와 worker 예약만 담당해야 한다. |

## 미완료

- 요청한 public-API test source와 `repro.diff`.
- 동일 owner, 다른 owner, gated owner 각각 20회 실행.
- .NET/Java 동일 시나리오.
- C++ Release tests ON, source binding build.
