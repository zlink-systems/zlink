# Java L5~L7 production profile

- 시작 2026-11-13 21:53 UTC, 제한 4시간. main worktree HEAD=3bd8756f99, branch=perf/4356-java-l57, clean. scratch는 git archive cffa8009d3. 모든 실행 CPU 6-7,14-15. measure.lock 미사용.

- profile baseline-L4-send: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/baseline-L4-send/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile before-L4-send: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/before-L4-send/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile before-L57-send: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/before-L57-send/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile baseline-L57-send: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/baseline-L57-send/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile baseline-L4-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/baseline-L4-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile baseline-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/baseline-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile before-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/before-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile pre-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/pre-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile c1-before-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/c1-before-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile delayed-before-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/delayed-before-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile startup-dump-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/startup-dump-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile baseline2-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/baseline2-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile valid-before-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/valid-before-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile c1-before-L4-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/c1-before-L4-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile c1-before-L57-send: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/c1-before-L57-send/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile c1-before-L4-send: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/c1-before-L4-send/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile baseline-c1-L4-send: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=False, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/baseline-c1-L4-send/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile c1-after-L57-send: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=True, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/c1-after-L57-send/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- profile c1-after-L57-bp: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=True, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/c1-after-L57-bp/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- 구현 전 보고: raw/report-before-implementation-v2.md. Production 4개 파일만 변경, 새 source·StateLane 검사 2개 추가. 기존 assertion 변경 없음.
- 변경 전 새 회귀 1건 실패(expected 1 owner, actual 8 callers), 변경 후 focused 114건 통과. 전체 core 19,962건(470 suites) 실패/오류/skip 0, fakeBackendTest NO-SOURCE, Java format PASS.
- 수정 후 C1 send 44,653 / bp 13,965건 모두 submitted=completed=received, errors=0. 비교는 costs-symmetric.json(양쪽 owner wait 포함). 전체 bp source CPU/alloc 감소는 확인하지 못함.
- 동일 JVM owner control: caller CPU 9.90–14.02us/turn·76.28–81.31B/turn, result 1.40–1.49us/turn·6.03–9.29B/turn. Throughput gate 판정 미실행.

- profile c1-L4-send-policy-confirmed: 3bd8756f993ee1f003321f46b68ad21d82ea5dec, dirty=True, scratch cffa8009d3, CPU 6-7,14-15; JAR hash /root/jobs/fx/cx/v21/s033/j57/raw/c1-L4-send-policy-confirmed/fingerprint.json. receive/reply와 추가 비용: call-shape.md.

- 최종 scheduling 감사: 초기 baseline-c1-L4-send(SCHED_IDLE)은 최종 표에서 제외하고 c1-L4-send-policy-confirmed로 교체. 최종 선택 6 run의 source/target start·active 전체 thread는 SCHED_OTHER, CPU 6-7,14-15, 모든 count 일치·errors 0·각 run 20초 미만. raw/final-precommit-verification-v2.json.

- 로컬 commit da040225847199e58661bfc6bbfda22264331320, branch perf/4356-java-l57, worktree clean. Push/PR 없음. 최종 source bytes=commit, 자체 Java/Gradle/profile process 0. raw/final-verification.json.
