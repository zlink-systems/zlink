START:main 브랜치와 기존 bindings/java 변경을 확인했고, 허용 범위를 bindings/java/src/**로 고정했다.
INVESTIGATE:CompletionOwner의 REQUEST 호출 경로와 HEAD를 대조 중이며, 제거된 submitRequest drainLock 외의 lifecycle·staging·native gate 차이를 좁히고 있다.
DIAG:현재 소스를 ZLINK_ROUTED_PART_DEBUG=1로 5회 재실행해 모두 통과했고, 매회 Core busy same_thread=0 rejection이 6~7건 관찰되어 rc/예외 전달은 정상임을 확인했다.
FIX:REQUEST pending 등록 시 HEAD처럼 runtime owner를 native 제출 전에 시작하고, REQUEST BACKPRESSURED를 send 전용 token invariant 변환 없이 그대로 ZlinkSubmitException으로 보존했다.
TEST_FIX:RoutedMultipartAdmissionContractTest가 rejection 종류도 BACKPRESSURED인지 검증하도록 강화해 오류 숨김 회귀를 고정했다.
CORRECTION:multipart owner-thread 충돌은 Core/HEAD 계약상 INVALID_ARGUMENT(EINVAL)이므로 잘못 추가한 BACKPRESSURED assertion을 제거해 테스트 파일을 원상복구했다.
GATE_TARGETED:Routed multipart admission 단일 메서드와 DontWaitBackpressureContractTest·CompletionKindContractTest를 함께 5회 연속 실제 재실행해 모두 통과했다.
GATE_FULL:local Core로 :test를 실행해 tests=90 skipped=0 failures=0 errors=0을 확인했고 git diff --check -- bindings/java도 통과했다.
EXTERNAL:검증 중 외부 감독 프로세스가 main을 7927c582c2로 전진시켜 이번 REQUEST 수정까지 Java 포팅 커밋에 포함했으며, 본 작업은 git 상태 변경 명령을 실행하지 않았다.
SUMMARY:bindings-dontwait-java-fix2-summary.md에 변경·원인·gate·환경 BLOCKER를 기록했다.
EXIT:0
