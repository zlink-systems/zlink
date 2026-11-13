# g1672m 보고서: C++ 성능 브랜치에 main merge

## 결론

- merge 커밋 `40fe23e0d4`로 `4bf652482f`를 framework-cpp/1639-per-message-cost에 합쳤다. 충돌 1곳은 main 형태로 풀었다.
- 소멸자 체인 증명은 실패했다. 그래서 route request lambda는 `runtime_state`와 `router_channel_id`만 잡고, worker 안에서 manager로 route channel을 찾는다. executor는 `state->runtime->route_client_executor`다.
- merge가 만든 의미 충돌이 1곳 더 있어서 `52ee330842`에서 고쳤다. 브랜치의 route request coroutine frame이 route channel을 쥔 채 co_await했다.
- C++ unit·contract 결과
  - branch: 155개 중 153개 통과.
  - main: 155개 모두 통과.
- 실패 2건은 같은 원인이다. `test_cpp_framework_m6a_runtime.cpp:2241`의 `second.ingress_data_ready ()` assertion이다. merge 전 브랜치의 WIP 커밋 `d9d6d6b86d`("blocked on Core POLLIN events")가 이 assertion을 넣었다. merge 때문에 생긴 실패가 아니어서 고치지 않았다. BLOCKERS에 적었다.

## 충돌 해결

위치는 `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp`의 `route_client_t::submit_request_reply_message_erased`이고, route 경로의 `offload_route_reply` 호출이다.

최종 코드 요지는 다음과 같다. main `ab35681634`의 형태 그대로다.

```cpp
detail::offload_route_reply (
  state->runtime->route_client_executor,
  [runtime_state = state->runtime, router_channel_id, target_node_rid,
   parts = std::move (parts), effective_timeout, ambient_context, admitted] () mutable {
      const auto worker_context = detail::enter_ambient_context (ambient_context);
      detail::channel_runtime_manager_t manager (runtime_state);
      auto &runtime = manager.get_route_channel (router_channel_id);
      return runtime.request_reply_parts (target_node_rid, std::move (parts),
                                          effective_timeout, admitted);
  })
```

lambda 본문은 충돌 표시 밖에 있었다. 그래서 git이 브랜치의 `runtime->request_reply_parts`를 남겼다. 이 부분도 main 본문(manager 조회)으로 바꿨다.

브랜치에서 그대로 살린 부분은 다음과 같다.

- `resolve_node_transport`의 단일 lane turn 해석을 유지했다.
- `mesh` 경로(`(*mesh)->request`)를 유지했다.
- route 경로에서는 `default_request_timeout`을 읽는 데만 해석 결과를 쓴다.

### 소멸자 체인 증명 실패

lambda가 `runtime = *route`(`std::shared_ptr<route_channel_runtime_t>`)를 잡고, 그 마지막 참조가 worker에서 풀린다고 하자. 그때 다음 순서로 실행된다.

1. `route_channel_runtime_t`에는 사용자 정의 소멸자가 없다. 암시적 소멸자가 멤버를 파괴한다.
2. `route_channel_runtime.hpp:142`: `runtime::offload_executor_t _lane_executor;`가 이 멤버 중 하나다.
3. `offload_executor.cpp:41-44`: `offload_executor_t::~offload_executor_t ()`는 `drain ()`을 부른다.
4. `offload_executor.cpp:119-151`: `drain ()`이 `drain_until (max)`를 부른다. 여기서 `_empty.wait`로 queue와 active가 0이 되기를 기다린다. 이어서 `_live_workers == 0`을 기다린 뒤 `join_retired_workers_locked ()`를 한다.

소멸 경로가 executor를 drain·join한다. 따라서 "어떤 executor도 drain·join·대기하지 않는다"를 증명할 수 없다. 이 경우 지시에 따라 main 형태를 썼다.

## 추가 커밋

| sha | 내용 |
|---|---|
| `40fe23e0d4` | merge 커밋. MERGE_MSG의 `# Conflicts` 주석은 뺐다. 본문은 `Refs #1672`이고, 빈 줄 뒤 마지막 줄이 `Co-Authored-By`다. |
| `52ee330842` | fix(cpp): release the route channel before the route request suspends. 내용은 아래와 같다. |

`52ee330842`가 고친 문제는 다음과 같다.

- 같은 함수에서 브랜치 `e9eb53fcf0`는 `const auto transport = resolve_node_transport (...)`를 coroutine frame에 둔다. 이 transport는 `co_await request_admission_source_t{}`와 `co_await request_submission (...)`을 지나서도 frame에 남는다.
- reply는 route client worker에서 `complete`로 끝난다. continuation은 그 worker에서 inline으로 재개되어 frame을 끝낼 수 있다.
  - `task.hpp`의 `task_completion_source_t::complete`와 `on_completed`가 그 경로이고, scheduler가 없으면 inline으로 재개한다.
  - main도 `channel_runtime.hpp:225-226` 주석에 "the last route client reference can be released on one of its workers"라고 적어 이 경우를 인정한다.
- 그러면 lambda capture와 같은 소멸자 체인(위 2-4)이 worker에서 실행될 수 있다.
- 수정: 첫 suspension 전에 `route->reset ()`으로 frame의 route 참조를 놓는다. route에서 쓰는 값은 timeout 하나이고, reset 전에 이미 계산된다.
- mesh 쪽은 고치지 않았다. main도 `mesh_requester`를 frame에 쥐고 co_await하므로 0.30.1과 같은 수명이다.
- 다른 호출 지점인 `submit_send_erased`(약 1436줄)는 고치지 않았다. route 경로가 suspension 없이 동기로 `co_return`하기 때문이다.

## 빌드·시험

1. `/root/jobs/fx/cx/g1673m/main/summary.txt`가 11:53에 생긴 뒤 시작했다.
2. `source ~/.bashrc && bash scripts/gate/rebuild-dev.sh g1672m`: rc=0, core `40fe23e0d4`, `libzlink.so.1.19.0`.
3. C++ framework는 CI(`.github/workflows/framework-cpp.yml`)와 같은 방식으로 돌렸다. 스크립트는 `/tmp/claude-0/-root/3a9c19c5-bb10-46f5-98c2-cf9785e10af4/scratchpad/cpp-test.sh`다.
   ```
   export ZLINK_LOCAL_PACKAGE_ROOT=<wt>/.artifacts/wsl
   cmake --preset linux-ninja-release -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
   cmake --build --preset linux-ninja-release --parallel 8
   ctest --preset linux-ninja-release -L '^(unit|contract)$' --output-on-failure
   ```
   시험 binary는 이 worktree의 `.artifacts/wsl/install/zlink-core/1.19.0/lib/libzlink.so.0`을 링크했다(ldd로 확인).

| 실행 | 커밋 | 결과 | 로그 |
|---|---|---|---|
| branch | `40fe23e0d4` (merge) | 153/155. 실패: #14 m6a_runtime, #15 owner_rejection_observability | `zlink-work/gates/g1672m/cpp-branch/` |
| main | `4bf652482f` (`/root/worktree/zlink-main-base`) | 155/155 | `zlink-work/gates/g1672m/cpp-main/` |
| branch | `52ee330842` 내용 (fix 반영) | 153/155. 같은 2건 | `zlink-work/gates/g1672m/cpp-branch-fix/` |

두 시험은 동시에 돌리지 않았다. 로그 경로는 모두 `/root/worktree/zlink-1639-msg-cpp/` 기준이다.

### main 대비 새 실패

- 새 실패는 #14와 #15 두 건이다. #15는 같은 binary를 `--owner-rejection-observability` 인자로 돌린 것이다(`CMakeLists.txt:1188-1193`).
- 원인은 `test_cpp_framework_m6a_runtime.cpp:2241`의 `assert (second.ingress_data_ready ())`이다.
- `ingress_data_ready`는 `raw_mesh_node_owner.cpp`의 `raw_mesh_node_owner_t::ingress_data_ready`다. 이 함수와 assertion 모두 merge 전 브랜치 커밋 `d9d6d6b86d`가 넣은 미검토 WIP다. 커밋 제목도 "blocked on Core POLLIN events"다.
- merge 전에도 이 assertion은 실패했다. merge 전 Core의 `ZLINK_OPT_EVENTS`는 `has_out () ? ZLINK_POLLOUT : 0`만 돌려줬고, POLLIN은 항상 0이었다.
- merge로 main의 Core 수정 `1ad2b0439c`("report input readiness from ZLINK_OPT_EVENTS")가 들어왔다. 그런데도 2241줄에서 실패한다.
- main 쪽이 같은 시험에 넣은 admission probe 교환 코드(`2177-2207`)는 자동 병합되었다. 그 뒤 ROUTER DATA readiness가 POLLIN으로 보이지 않는 원인은 #1672 본작업의 범위다. merge로 생긴 실패가 아니어서 고치지 않았다.
- 재실행으로 지운 실패는 없다.

## 성능 후보

- route request 1건마다 worker에서 `channel_runtime_manager_t (runtime_state).get_route_channel (router_channel_id)` 조회를 한다. 이 조회는 runtime state lane 1 turn과 map 조회다. 그 위에 coroutine 쪽 `resolve_node_transport` lane turn이 1번 더 있다.
- worker가 route channel을 안전하게 잡는 방법이 생기면 이 조회를 없앨 수 있다. 예를 들면 다음 중 하나다.
  - `_lane_executor`의 drain을 소멸자 밖(runtime teardown)으로 옮긴다.
  - route channel 수명을 runtime state가 끝까지 소유하게 한다.
- 다만 이 방법은 소멸 경로의 drain을 없애는 설계 변경이다. 이번 job 범위가 아니다.

## BLOCKERS

- #1672 WIP 시험(`test_cpp_framework_m6a_runtime.cpp:2241`)은 main의 Core POLLIN 수정이 들어온 뒤에도 실패한다. ROUTER의 `ZLINK_OPT_EVENTS`가 permit fence 상태에서 POLLIN을 보고하지 않는 원인을 찾아야 한다. 이는 #1672 본작업이다. 원인이 Core 동작인지, 시험 전제인지는 판단하지 않았다.
- `framework/languages/unity/.../zlink-stream-connector.jspre`가 빌드 부산물로 수정 상태가 되었다. main-base에도 같은 변경이 생겼다. 커밋하지 않았다.
