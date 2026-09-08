# MP-1 진행

- 시작: 2026-09-07 15:41 KST. 상한: 16:26 KST(45분).
- 범위: 분석·설계만. 소스 패치·커밋·스펙 수정 없음.
- 기준: main `33ae2be045`, detached worktree `/home/hep7hep7/project/zlink-work/mp1` 생성. main의 기존 변경은 보존.
- 15:42 KST: 공통 규칙, 버그 보고서, D-BP12/D-B197, socket 계약을 확인했다. caller-local 통일은 HWM뿐 아니라 기존 control boundary와 staging 수명도 함께 검토해야 한다. 실제 API 분기·mapper·테스트를 확인 중.
- 빌드·테스트·성능 측정: 미실행(설계 전용).
- 15:44 KST: 현재 main 코드에서 전제를 정정했다. PAIR/DEALER/ROUTER SEND MORE는 이미 `handle_state_t::send.buffered_parts`에 보관되고 caller-local이 아니라 socket당 단일 slot이다. owner_thread 검사와 lifecycle marker 양쪽이 충돌을 거절한다. mapper는 EINVAL을 보존하며 DONTWAIT scope 진입 경로가 별도로 EAGAIN으로 바꾼다. control 조항의 실제 소유 위치와 HWM 예외·회귀 테스트를 조사 중.
- 15:47 KST: 두 설계의 계약 차이를 분리했다. A는 socket-owned per-caller staging + 기존 complete admission 재사용을 권고하는 방향이며, control boundary를 물리 write 구간으로 한정하려면 별도 D 승인이 필요하다. B는 MORE wait-token 계약 추가와 marker/complete 해제 wake가 필요하다. 기존 충돌 거절 unit test 2개, control coalescing integration test를 확인했고, public HWM test는 이미 MORE 성공/FINAL 실패를 명시한다. 빌드 없이 정적 근거로 보고서를 작성 중.
- 15:57 KST: 설계 보고서 초안 308행 작성 완료. A 권고, D1~D6 계약 차이, 구현 파일/함수, 기존 test 영향, 신규 4-thread/REQUEST/REPLY/close/TSan 목록과 hotpath 5셀 추정을 포함했다. marker 제거만으로 MORE lifecycle RMW가 줄지는 않음을 정정했다. 초안 작성 중 진행 파일의 3분 갱신 간격을 지키지 못한 구간(15:47~15:57)이 있다. 지금 인용 위치·표 형식·작업 범위 최종 점검 중.
- 15:59 KST 완료: 보고서 314행. 표 열 수·공백·인용 source 경로 점검 완료. mp1 worktree `git status --short` 출력 없음, `git diff --exit-code` 성공. 소스·스펙·공개 인터페이스·테스트·commit 변경 없음. 빌드/ctest/TSan/성능 측정 미실행. 시작 후 약 18분으로 45분 상한 내 완료. 결과는 지정된 design/progress 두 문서에만 남겼다.
