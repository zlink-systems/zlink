# review-MP-2 진행

- 시작: 2026-09-07 18:01 KST. 상한 45분.
- 범위: mp2 미커밋 diff 24개 파일(+1512/−509), main 미커밋 계약, MP-1·D-B197/198·구현 보고서 정적 대조.
- 제한: 소스·스펙·테스트 수정, 빌드·실행, 커밋 없음. 보고서·진행 파일만 작성.
- 18:02 KST: 공통 규칙과 설계의 수명·lock·성능 기준 확인. caller slot과 SEND·REQUEST·REPLY 진입 경로 추적.
- 18:04 KST: 같은 REPLY token의 중복 제출을 ENOENT로 변경한 코드·테스트가 main README:1131의 EBUSY 계약과 불일치함을 확인. 만료 slot 해제의 physical sync 범위, TLS 첫 할당 예외, helper mutex 추가 비용을 검토 중.
- 18:07 KST: physical sync 안의 만료 payload 해제, TLS 조회의 미포착 bad_alloc, close의 추가 allocation 실패 경로 확인. REQUEST observer의 registry→pipe lock 및 commit-before-flush는 유지됨. 새 동시 테스트는 NONE/inproc/2-part에 집중하며 payload 길이 오류를 건너뛰는 assertion과 completion ID 대조 누락을 확인.
- 18:10 KST: logical RID 제거가 staged REPLY의 registry slot 반환을 지연시키는 회귀 확인. 초기 validation abort는 public-handle pin을 먼저 놓은 뒤 REPLY context를 해제하므로 close와 socket 수명 경쟁이 생김. pipe pin이 socket pin을 대신하지 않는 경로까지 대조. 보고서 작성 중.
- 18:16 KST: 보고서 초안 작성 완료(B 6, W 7, S 2). main 계약 행 번호와 mp2 재현 경로를 재대조하여 인용 수정. diff는 최초와 같은 24개 파일 +1512/−509. 빌드·실행·소스 수정 없음.
- 18:17 KST: 최종 보고서 완료(B 6, W 8, S 2). Message:108의 PUB/XPUB 적용 범위 문구도 비차단 항목으로 추가. 최종 판정은 차단 6건으로 채택 불가. 변경 파일은 review-mp2.md와 이 진행 파일뿐이며 빌드·실행·커밋 없음. 소요 약 16분, 상한 45분 이내.
