# review-CCU-3 진행

- 2026-09-08: 검토 시작. 공통 규칙, 1차 리뷰, CCU-3 수정 보고서와 저장소 상태를 확인했다.
- 현재 단계: 미커밋 diff와 Auto-HWM 스펙/POSDDD 대조, B-CCU2-1/2/3 정적 재검증.
- 제한 준수: 소스·스펙·테스트 수정 없음, 빌드·실행·커밋 없음.
- 중간 판정: B-CCU2-1의 고정 배열/예외 매핑과 B-CCU2-3의 registry 복제 상태 삭제는 확인했다. B-CCU2-2의 apply→record 순서도 확인했다.
- 신규 후보: 증분 성공 뒤 기존 방향 인하를 수행할 debounce 재계산이 예약되지 않는다. 신규 테스트 대부분은 새 socket 생성이 만든 pending generation 때문에 증분 경로가 아니라 full fallback을 비교하며, 공통 비교 helper의 `total_applied >= full` 완화도 fallback 케이스까지 적용된다.
- 다음 단계: detach/lock-order 반례와 동시 attach 결정성, 플랫폼·공개 헤더 범위를 최종 정리한다.
- 최종 판정: B-CCU2-1/2/3 자체는 해소. 신규 B-CCU3-1(증분 성공 뒤 debounce 수렴 예약 없음) 1건으로 채택 불가.
- 테스트 판정: W-CCU2-1 미해소(대부분 full fallback), D-H1 완화 범위 과다, concurrent attach 경쟁 비결정적. W-CCU2-2는 부분 해소.
- 범위 확인: `core/include/**`, `core/src/libzlink.vers` 변경 없음. 정적 플랫폼 비호환 없음. 리뷰 보고서를 `review-ccu3.md`에 작성 완료.
- 제한 준수 완료: 소스·스펙·테스트 수정 없음, 빌드·실행·측정·커밋 없음.
