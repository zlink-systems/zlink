START main 브랜치와 bindings/java 기존 변경 범위를 확인함
DIAG Java diff, Core EINVAL 조건, 테스트·JDK 환경을 병렬 조사 시작
ROOT_CAUSE ControlWake.setLingerZero가 legacy LINGER 17을 Core 공개 ID 0x300A로 변환하지 않아 EINVAL 발생
FIX CompletionOwner wake PAIR의 LINGER 설정에 기존 SocketOptionRouter 변환 경로 적용
TEST 대표 PullCompletionContractTest 루트 task는 통과했으나 잘못 지정한 aggregate test filter가 perf-multi에서 no-tests로 종료
