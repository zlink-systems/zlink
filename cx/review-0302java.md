# 0.30.2 Java 묶음 감독자 리뷰
- 2026-10-16 #1717 a3b5414dd4 승인: markDelivered를 claim(:447-450)으로 이동, nextTick은 계산만. 스펙 10-spot-timer :59-68(실제 시작 기준 번호) 대조. red(scheduledIndex [1,1,1]) 로그, 전체 3,132 실패 0 확인.
- #1730 보류 → 감독자 선택: 대안 1(infrastructure await에 명시 Yield, queue의 미완료 lifecycle 자동 Yield 제거; .NET YieldFrameworkCallAsync와 같은 모양). 단, ZLinkSerialExecutionQueue·lifecycle 경로가 #1873과 겹치므로 #1873 승인·머지 뒤 착수.
- #1826 보류 → 감독자 선택: 대안 2(기존 descriptor/admitted role snapshot을 논리 분류 owner 한 곳에서 사용, 알려진 Client → NotFound, role 미상(manual·descriptor 없음)은 기존 transport 결과 — topology :465-481). c1826 job 결과와 합쳐 진행.
- #1826 b4d38fa63d 승인 → PR. diff 전 줄 확인(role 필드 전달·fallback 한 곳), 시험 3분류, 3,040+91 실패 0 로그.
- #1726 Java daca4f535a 승인→PR(replacePeerConnection 한 곳 판정 diff 확인, registry 시험은 대기 조건만 강화·단언 유지, 3037+91+171+3). job이 공유 ~/.cache/zlink/native 사용 가능 실행 1건 자진 보고. Kotlin timeout 2건 원인 미확정.
- #1882 be4ba7d8b1 승인→PR. source unbind 제거·동일 generation route 변경 허용(낮은 generation 거부 유지) diff 확인, main 원본 red, 3304 실패0.
