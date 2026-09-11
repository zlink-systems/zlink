# sync-impl-inventory 진행

- 상태: 완료
- 범위: socket turn, receive, pipe, mailbox, Auto-HWM, STREAM/Asio, WS/WSS, lock 순서
- 제약: 읽기 전용 조사. 소스·스펙·테스트 수정 및 빌드·실행·커밋 없음.
- 결과: `sync-impl-inventory-0174.md`에 항목별 현재 구현 규칙과 파일:행 근거를 기록함.
- 확인 불가: close의 RID 명시적 clear, 모든 virtual command destination 아래의 전역 lock-order 완전 열거, 동적 경합 수치.
- 검증: 정적 소스 대조만 수행. 빌드·테스트·실행 없음.
