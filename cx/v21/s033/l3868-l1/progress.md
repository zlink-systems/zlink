# #4350/#4351 C++ L0·L1 진행

첫 산출물: [call-shape.md](call-shape.md). Branch `exp/3868-cpp-ladder`, 시작 HEAD `73285401ca`, 시작 시 작업 트리는 clean이다. 시간 한도는 raw/task-time.json에 기록했다.

## 결정 소유 대조

| 결정 | spec 소유 조항 | 현재 판정 위치 | 복원 소유자 |
|---|---|---|---|
| receive 전 permit | application job queue §3.1–2 | C++ application_job_queue.hpp:568; Java ZLinkApplicationJobQueue.acquireBlocking(REMOTE), bench server:141; .NET Program.cs:166 AcquireAsync(Remote) | 실제 C++ application_job_queue_t |
| handler 직전 반환 | 같은 spec §3.5 | C++ application_job_queue.hpp:1281 handler_entry_t; Java bench server:148 handlerStarted; .NET Program.cs:172,179 ReleaseForHandlerStartAsync | 실제 C++ permit_t::release_for_handler_entry |
| 물리 receive·reply·completion | binding/Core | raw와 L0 공통 binding 호출 | 변경 없음 |

Java·.NET 참조와 같은 수신 전 확보·handler 직전 반환이다. 사용자 승인 범위는 실험 전용 L1 permit 복원이며 제품 runtime의 A/B 변경을 주장하지 않는다(C). L0의 fixed handler와 공통 source submit을 유지한다. 직접 queue 호출과 handler_entry_t wrapper를 비교해 직접 호출을 선택했다. Wrapper는 std::function 생성과 callback 이동을 추가해 L5/L6 비용을 섞는다.

L1 permit authority 판정 위치는 0→1(application_job_queue_t), 물리 admission/completion은 1→1(Core/binding)이다. 별도 queue 상태·counter·lock은 추가하지 않는다. 실제 queue가 소유한 상태를 재사용한다.

- 2026-11-13T13:51:05Z L1 commit `608513f260`; 지정 cpp-build-slot 요청. 실제 compile 전 shared measure gate 대기 중이다.

GATE-WAIT 30.7

GATE-WAIT 31

GATE-WAIT 45.1

GATE-WAIT 60.8

GATE-WAIT 74.8

GATE-WAIT 89.8
