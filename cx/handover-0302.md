# kairos → zlink 0.30.2 인계 목록 (2026-10-16)

결과 위치: 각 job의 보고서 /root/jobs/fx/cx/<job>/report.md (job 이름은 표에). job이 끝나면 kairos가 완료 알림을 받아 보고서 경로와 요약을 #zlink에 넘깁니다. push·PR은 zlink가 합니다(예외: Java PR 5개 job은 gate 통과 시 push·PR 코멘트까지 하도록 이미 지시됨).
공통: C++ 빌드는 /root/jobs/fx/cx/cpp-slot.sh, Java gate는 /root/jobs/fx/cx/java-slot.sh로 머신 전체 2개씩 제한. 모든 job에 판정 소유 표·수정 전/후 개수 요구함.

| 이슈 | 언어 | worktree | 브랜치 | 상태 | 남은 판단 |
|---|---|---|---|---|---|
| #1979 | Java | /root/worktree/zlink-c1979 | framework-java/1979-relocation-capacity | job 실행 중(최신 main 재merge·전체 gate) — 설계는 감독 리뷰 승인 | 업그레이드 중 옛 빌드가 prepare한 aggregate는 예약이 없어 commit/abort가 'capacity record is inconsistent'로 멈춤(진행 중 relocation만). 허용 방법 추천만 받기로 함. Node·C++ Redis는 결함 없음 |
| #1786 | C++ | /root/worktree/zlink-c1786 | framework-cpp/1786-deferred-join-first-push | job 실행 중(최신 main 재merge·C++ 전체 gate), PR 없음 | 수정 커밋 266a9615ae, 이전 gate 153/153 |
| #1783 | C++ | /root/worktree/zlink-c1783 | framework-cpp/1783-add-subscribe | PR #1948 열림, 같은 job이 이어서 cpp-all-stage+샘플(Bingo·TicTacToe·ZoneWorld) 실행 예정 | zlink 리뷰 요구 결과 첨부 |
| #1961 | .NET/Java/Node/C++ | zlink-c1961 / zlink-j1961 / zlink-n1961 / zlink-p1961 | framework-dotnet/1961-timer-close-before-publish, framework-{java,node,cpp}/1961-timer-hold-until-ready | job 실행 중(스펙 #1973 구현) | — |
| #1877 | .NET | /root/worktree/zlink-c1877net3 | framework-dotnet/1877-relocation-actor-inventory | job 실행 중(update Actor가 relocation inventory에서 빠짐, border-tick 'SPOT application admission is sealed') | sass 로그 /root/jobs/fx/cx/c1877net3/logs |
| #1877 | Java | /root/worktree/zlink-c1877javadi | framework-java/1877-pending-tick-delivery-index | codex job 실행 중(pending tick DeliveryIndex를 golden +1로) | — |
| #1880 #1885 #1889 #1893 #1897 | Java | zlink-0302-java / zlink-c1826 / zlink-c1726java / zlink-c1876java / zlink-c1887 | 각 PR head | job 실행 중(최신 main 재merge·gate·통과 시 push+PR 코멘트) | 이전 결과 /root/jobs/fx/cx/itest3/report.md |
| #1882 | Java/.NET/Node/C++ | zlink-c1882-{java,net,node,cpp} (+ 임시 -java-red, -net-red) | framework-<lang>/1882-commit-membership | job 실행 중(commit 시 source membership 이전, binding 기록 generation 정리(B→A→B), catch-all 분류 축소) | 진단 /root/jobs/fx/cx/c1883java/report.md Part B, /root/jobs/fx/cx/reg1877jvm/report.md |
| #1879 | C++ | /root/worktree/zlink-c1879cpp | framework-cpp/1879-relocated-view-reply | job 실행 중 | — |
| #1947 | C++ | /root/worktree/zlink-c1947cpp | framework-cpp/1947-empty-host-drain | job 실행 중(Java는 #1978 머지) | 머지되면 #1947 닫기 |
| #1976 | Node/.NET/Java/C++ | zlink-c1976 / -dotnet / -java / -cpp | framework-<lang>/1976-concurrent-yield | job 실행 중(스펙 #1977) | — |
| #1872 | .NET/Java/Node/C++ | zlink-c1872-{net,java,node,cpp} | framework-<lang>/1872-reclaim-dead-members | job 실행 중(C++ 마무리·네 언어 gate) | 옛 진단 worktree zlink-c1872net은 미사용(정리 대상) |
| #1962 | .NET/Node | zlink-c1962 / zlink-c1962-node | framework-dotnet/1962-startup-store-stall, framework-node/1962-preparing-until-lease | job 실행 중(Preparing 상태·status 보존·gate·SpotClose 간헐 비교) | 남은 것: lease 있는데 descriptor 쓰기 실패(§10), claim 응답 유실 재시도 충돌 |
| #1883 | C++ | /root/worktree/zlink-c1883cpp | framework-cpp/1883-binding-absent | job 실행 중(Node·.NET·Java 머지됨) | 머지되면 #1883 닫기 |
| #1989 | C++ | /root/worktree/zlink-c1989 | framework-cpp/1989-di-singleton-once | codex job 실행 중 | HTTP 처리량 전후 비교(시간 측정은 단독) — 성능 측정이라 kairos가 머신이 조용할 때 맡음 |
| #1730 | Java | /root/worktree/zlink-c1730 | (#1899 위에 쌓임) | codex job 완료·리뷰 안 됨(/root/jobs/fx/cx/c1730/report.md) | #1899(#1873, 0.31.0) 머지 후 진행 |
| #1775 | 네 언어 | zlink-c1775n, zlink-c1775r2(옛 진단) | — | 스펙 PR #1953 sol 리뷰 결과 미확인(/root/jobs/fx/cx/s1775/codex.log) | 사용자 결정: 추천 1(target이 relay-ready 전 Session route 확보). 스펙 머지 후 네 언어 구현 미착수 |
| #1988 | Node 시험 | — | — | 미착수 | — |
| #1942 | Java TLS 시험 | — | — | 미착수 | — |
| #1788 잔여·#1784 | C++ | — | — | 미착수(queue c1788r2) | /root/jobs/fx/cx/backlog/queue.md |
| #1878 | C++ | — | — | 미착수(source_preserved 오분류) | queue.md |
