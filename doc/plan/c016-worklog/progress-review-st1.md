# review-ST-1 진행

- 2026-09-08 06:12 KST: 리뷰 완료. 보고서 `review-st1.md`의 근거 행과 소유권 호출 경로를 최종 대조했다. 차단 2건, 현재 diff 채택 불가. 40분 상한 안에 종료.
- 일반 blocking receive는 실패한 시도의 lease를 놓고 대기함을 확인. whole-record continuation은 직접 비대기 xrecv를 사용한다.
- B-1: 임시 `receive_owner_async` 해제 뒤 mutex 전용 count-1 completion drain과 lease 전용 ROUTER 수신의 동시 진입 확인. DATA receiver와 completion owner가 각각 하나인 구성이다. async 설치에서 임시 상태를 차용하는 경계도 같은 항목에 기록.
- B-2: 무조건 포함한 POSIX 헤더와 CMake 공통 등록 때문에 Windows 컴파일 호환성 문제. 빌드 없이 소스로 확인.
- 기존 readiness/control attach 경계, yield 공정성 한계, 성능 주장 조건, stress 테스트의 결정성·실패 종료 상한을 W/S 항목으로 기록. 원 보고서의 실행 기록과 직접 읽은 suite/TSan 로그를 구분했다.
- 소스·스펙·테스트 변경, 빌드·실행·커밋 없음. 지정 보고서와 이 진행 파일만 작성한다.
