# POSDDD refactor 2 progress

- 2026-09-04: `perf/phase2-judge` 브랜치와 clean 작업 트리를 확인했다. 필수 POSDDD 원칙, Core POSD module/source layout/hot-path 계약, 최근 6개 커밋과 HEAD stat을 읽기 시작했다.
- 2026-09-04: `posddd.ko.md` 전체와 ZLink 시스템·소스 주석 원칙, Core 07/08/10 문서를 읽었다. 최근 runtime 변경 `8b6c2aa906`의 stat과 담당 범위 diff를 확인했고, lifecycle·PAIR whole-record·나머지 범위의 독립 정찰을 병렬로 시작했다.
- 2026-09-04: `wait_submit_progress`에서 timeout 예산, waiter/public-command-owner lease, PAIR async-owner 대여, retained non-PAIR owner 준비를 책임별로 분리했다. TLS는 재진입 self-wake만 억제하려면 socket별 thread identity가 필요함을 전 트리 호출로 확인해 lease 내부에 유지했다. owner retire epoch 명칭도 실제 전이에 맞췄고, 중간 전체 build가 통과했다.
- 2026-09-04: PAIR whole-record의 success-only 경로와 authoritative frame fallback 경계를 명확히 하고, 단일 호출 lambda·도달 불가 errno 분기·message copy/close 사본을 제거했다. `drive_send_pending`의 backpressure 대상 추적은 호출별 의미를 보존하면서 socket-owned scratch를 재사용하도록 바꿔 hot path의 매번 `std::set` node 할당을 없앴다. 최신 수정 중 실행 중이던 중간 build는 결과 혼동을 막기 위해 중단했으며 최종 build에서 다시 검증한다.
- 2026-09-04: `send_completion_submit_blocking`과 `request_admission_submit_blocking`을 timeout context, fast path, target selection, commit된 admission 대기 책임으로 분리했다. lifecycle pin, waiter 게시, async owner 대여, errno와 multipart consume 시점은 기존 순서로 유지했고 관련 TU 증분 컴파일이 통과했다.
- 2026-09-04: 전 트리 참조 검사를 근거로 common의 선언/정의만 남은 함수, 닫힌 dead chain, write-only 필드와 자동 HWM role dead cache를 제거했다. FQ/LB/DIST/DEALER/ROUTER의 반복 상태 전이와 pass-through를 소유 helper로 모았으며, FQ read-miss의 publish-before-rotation 순서는 별도 helper로 보존했다. 공개 send 성공 시 recovery cache 두 항목을 함께 해제하는 게시 지점도 한 함수로 통합했다.
- 2026-09-04: 전체 gate를 완료했다. Core build 238/238, ctest 134/134, `git diff --check`, raw mirror cmp 12가 모두 green이다. 알려진 snapshot accounting test도 첫 실행에서 통과했다. 최종 변경량과 삭제 근거, 책임/명명 분리, commit staging 목록, 범위 밖 blocker를 summary에 기록했다.
