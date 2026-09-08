# G-10 진행

- 2026-09-07: 공통 규칙·G-10 브리프·G-A 프로파일 확인. `origin/main` a2b087ff0c에서 detached worktree 생성. 호출자 및 스펙 근거 조사 시작.
- 2026-09-07: 기존 G-A callgrind에서 `clock_gettime`을 Core `clock_t::now_us`와 벤치의 `steady_clock::now`로 분리. STREAM 즉시 성공마다 `submit_timeout_budget_t`가 deadline을 선계산하는 경로를 확인. baseline용 release lib 빌드 시작.
- 2026-09-07: worktree Release+LTO lib와 STREAM zlink 축소셀 바이너리 빌드 완료(JOBS=4). `clock_t::now_ms()`는 rdtsc 1,000,000 tick/2 안에서 캐시하고 miss만 `now_us/clock_gettime`으로 내려가는 구현임을 확인. baseline callgrind 준비.
- 2026-09-07: baseline 축소셀 완료: 60,873 msg, idle 차감 9,354 Ir/msg, `clock_gettime` 1.720/msg(Boost/Asio `steady_clock` 1.185, Core `clock_t` 0.534). 기존 deadline state를 sentinel로 재사용해 positive SNDTIMEO deadline을 첫 admission 실패 뒤 최초 wait에서 계산하도록 변경; 새 옵션·플래그·상태 없음.
- 2026-09-07: dev 빌드 성공. 지정 필터 49개 테스트를 5회 실행해 245/245 PASS. 다음은 동일 worktree `build-gate`에서 pristine before와 패치 after의 hotpath 5셀을 각각 측정.
- 2026-09-07: Release+LTO hotpath 5셀 before→after 완료: DD 3271.778→3231.215, DR reqrep 18861.654→18734.949, PAIR 2330.398→2287.624, RR tcp 2916.505→2874.050, STREAM tcp 14457.626→14421.722 Ir/msg. 5셀 모두 개선; reference 대비 DD가 0.9438로 개선 과다 판정 FAIL, 나머지 PASS.
- 2026-09-07: 최종 축소셀 9,354.276→9,314.626 Ir/msg, 전체 `clock_gettime` 1.7195→1.4850/msg, Core 기인 0.5344→0.3052/msg. 인라인 `rdtsc` dump caller 분해 완료. 최종 patch 상태 release 재링크, 보고서 작성 완료.
