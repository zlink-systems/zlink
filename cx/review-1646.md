# #1646 리뷰 (감독자, 2026-10-15) — 원래 브랜치 diff 9개 파일 전부 (merge 해결분은 별도)
판정: 반려(수정 후 재리뷰). 스펙 gap 없음. 방향(1ms hashed timer wheel, 등록 O(1), 조기 만료 없음, 요청별 timer 제거 — hot-path E3; .NET 완료 권한 원자적 꺼내기 한 곳 — submit-and-completion §10)은 승인.

F1 (§4.2 "시간 상한 안에 끝나지 않는 관리 작업은 다음 회전으로 넘긴다" 위반): 시간 상한이 첫 만료 뒤에 시작해서, 만료 없이 rounds만 줄이고 넘기는 항목을 훑는 동안 상한이 없다. timeout이 수 초인 일반 요청은 대부분 rounds>0이라 in-flight가 많으면 한 관리 작업이 무한정 길어진다.
 - Java ZLinkServiceOperationRegistry.expire() (while 조건 `expired == 0 || ...`, startedNanos는 첫 만료 때 설정)
 - C++ operation_registry.cpp expire() (같은 구조)
 - .NET ZLinkManagedMeshNode.TakeExpiredOperation() while(true) 안 rounds 건너뛰기, ExpireOperations()의 상한은 첫 take 뒤
 고칠 것: 상한을 관리 작업 진입 시점부터 센다(항목 방문 — 만료든 rounds 감소든 — 마다 상한 검사). 다음 회전에서 cursor부터 이어 간다. 새 상태·타이머 추가 없이.
F2 (동작이 아니라 소스 문자열을 검사하는 시험 — 구현 모양에 묶이고 상수 시간을 보장하지 못함): 지우거나 동작 시험으로 바꾼다(예: pending N이 커져도 등록·해제·만료 1건의 방문 수가 일정함을 공개/내부 계수로 확인).
 - Java deadlineLinkAndUnlinkUseFixedPointerWork
 - C++ test_cpp_framework_operation_registry.cpp main() 첫 블록(소스 파일 읽기)
 - .NET OperationDeadline_DoesNotAllocateARequestTimer, OperationDeadline_LinkAndUnlinkUseFixedPointerWork
F3 (CONTRIBUTING §5 — 머신 속도·sleep에 기대는 시험): 결정적으로 바꾼다(주입한 시계로 상한 경과를 만든다 — Java 시험이 쓰는 방식처럼).
 - C++ budgeted 블록: 50000건이 receive_batch_time 안에 다 끝나면 `expired < pending_count`가 깨진다.
 - .NET OperationDeadline_PreservesRoundsAndResumesABudgetedSlot: token.Register(() => Thread.Sleep(3)).
 F1 수정에 맞춰 "rounds만 있는 항목을 훑다가 상한에 닿으면 넘기고 다음 회전에 이어 간다"를 세 언어 시험에 추가한다.
남은 완료 조건: Java/Kotlin request-backpressure source CPU·처리량 전후 비교(감독자가 단독 측정).

## 재리뷰 (감독자, 2026-10-15 저녁) — 41ff29daf3·b7122613bf·1bd75f44eb + merge 4dcfd817fe·821116b63f
- F1 승인(Java·C++·.NET): 상한을 진입 시점부터, rounds-only 방문 포함 매 방문 확인. 새 시험이 rounds-only 8개를 due 항목 앞에 두고 주입 시계로 상한 소진 → cursor 재개를 확인(수정 전 코드에서 실패).
- F2 승인: 소스 문자열 시험 4개 삭제 — 동작 시험은 방문 counter라는 새 상태를 요구(AGENTS §3).
- F3 승인: C++·.NET 주입 시계. Java oneMaintenanceTask 시험의 scheduler 정지는 운영에 없는 동시 expire 경쟁 제거, 단정 불변.
- merge 4dcfd817fe 승인: 수신 루프는 main(#1638) 구조 + wait bound·ExpireOperations, local spot request는 main(#1745) caller 취소 유지, PendingOperation.Cancel은 원자적 꺼내기(TryRemove) 뒤에서만 호출됨 — 호출 8곳 모두 확인(1657,3302,4186,4203,5024,9606,10777,10789).
- F4 (새, .NET 성능): VisitOperationDeadline을 방문마다 RunOperation(lane turn — CurrentLane AsyncLocal 설정·복원, ExecutionContext 할당)으로 부른다. in-flight가 많으면 tick당 rounds-only 방문 수백 번이 각각 lane 진입. Java(synchronized)·C++(lock_guard)는 방문당 가벼운 lock. 상한 확인을 한 lane turn 안의 루프로 옮기고, 만료 operation은 재사용 목록에 모아 lane 밖에서 Complete한다(새 상태는 재사용 목록 필드 하나 — 기존 per-visit tuple 반환을 대체).
- 미확인: C++ format(clang-format-18 이제 설치됨).
- 2026-10-15 main merge d5184515c1 감독자 리뷰 승인: ZLinkManagedMeshNode.cs 충돌 2곳 — main의 DefaultServiceRequestTimeout은 branch의 단일 effectiveTimeout으로 유지, main 쪽 중복 선언 제거. .NET 2989·Java core 2521·C++ framework-unit 119 실패 0 (mainmerge-1015/).
