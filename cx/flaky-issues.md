## #3301 test(cpp): LocalEnqueueAllocations.SuccessfulLocalMessagesOnlyAllocateTheirPublishedRecordStorage가 머지 큐에서 간헐 실패한다
- `test_cpp_framework_local_enqueue_allocations.cpp:211` Expected: true. batch a4b6871402(run 41146598045, main c52ae3e + .NET 전용 #3259)에서 실패, 같은 main + #3259인 batch 9e66e0ff45(run 41143549309)에서는 통과.
- 할당 수를 정확히 세는 시험이라 #3241(process telemetry worker) 머지 뒤 비동기 전달 할당이 측정 구간에 섞일 가능성을 먼저 확인해야 합니다.
- AGENTS.md §4.3 간헐 실패 규칙으로 등록. #3259는 C++을 건드리지 않아 다시 넣습니다.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

## #3296 test(dotnet): StatefulServiceRuntimeTests.ControlSend*_KeepsAdmissionUntilSelectedRouteEnds가 osx-arm64 머지 큐에서 간헐 실패한다
- 시험: `ControlSendTerminated_KeepsAdmissionUntilSelectedRouteEnds`(batch 9e66e0ff45, run 41143549326, osx-arm64, `ZlinkSubmitException: zlink error code 8`), `ControlSendNotConnected_KeepsAdmissionUntilSelectedRouteEnds`(batch de9b367185 = #3277, run 41128210009 — #3259 없음)
- #3259 브랜치 Linux에서 ControlSend 계열 3건을 15회 반복: 15/15 통과
- 바꾼 코드와 무관하게 batch를 떨어뜨림. AGENTS.md §4.3 간헐 실패 규칙으로 등록

🤖 Generated with [Claude Code](https://claude.com/claude-code)

## #3293 test(dotnet): RemoteSpotFirstRequestTests.Actual_connection_refusal_ends_request_before_its_deadline가 머지 큐에서 간헐 실패한다
- 시험: `Zlink.Framework.UnitTests.Runtime.RemoteSpotFirstRequestTests.Actual_connection_refusal_ends_request_before_its_deadline` (5 s)
- 최근 머지 큐 Framework .NET 실패 6회 중 3회가 이 시험

머지 큐 batch에서 PR 변경과 무관하게 실패해 batch를 떨어뜨렸습니다(hep7 머지 큐 조사, 2026-10-31). 재발 방지 계획 2장 4번: 간헐 실패 시험은 이슈로 등록하고 고칠 때까지 추적합니다.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

