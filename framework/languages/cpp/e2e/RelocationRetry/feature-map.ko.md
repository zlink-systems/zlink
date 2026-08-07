# C++ RelocationRetry 검증 범위

이 runner는 `CPP-RELOC-001`의 public `app_t::relocate()` 재시도 계약을 설치 package와 두 process로
검증한다. Source process의 첫 호출은 replacement가 없어서 `Blocked/TargetUnavailable`로 끝나며,
source가 `Serving`을 유지하는지 확인한다. 이후 같은 Redis Location Store에 target process를 게시하고
같은 source process에서 두 번째 호출을 실행한다. 두 번째 호출은 이전 blocked 결과를 재사용하지 않고
`Relocated/None`으로 완료되어야 한다.

Runner는 빈 prefix에 Framework package를 설치하고, source tree include를 사용하지 않는 out-of-tree
consumer를 build한다. 실행 log에는 설치한 `libzlink_framework.a`의 SHA-256과 두 relocation terminal을
기록한다. Spot state transfer, application-signaled readiness와 participant 복구는 이 runner의 범위가
아니며 각 공통 relocation scenario에서 별도로 검증한다.
