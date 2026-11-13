# Node direct JSON writer 조사

- Primary: perf/4354-node-json-writer, f723033a5a524af5dcfa4c04fc42677596c5f429, clean. 사용자 지정 branch 확인. internal-docs.sh, GitHub, sub-agent, measure.lock 사용 없음.
- 공통 규칙과 n4json/n4json2 보고서 읽음. 사용자 최신 조건이 측정 lock/CPU 조건보다 우선한다. 모든 build/test/profile은 CPU 6-7,14-15. Profile process lifetime ≤20초, shape 비교만 수행한다.
- Scratch: /root/worktree/zlink-v21-s033/n4w-scratch, ae8a5178b5 archive + primary packages. 다른 worktree/output 사용 없음. local http-client 누락으로 최초 materialize 실패(raw/materialize-http.log), 이 scratch에서 registry binding과 bootstrap compiler로 package를 생성한다. Raw 삭제 없음.
- 후보: single walk, native JSON.stringify 없음, fallback 없음. JSON.stringify의 toJSON → 원본 재조회/사용자 replacer → 정책 → boxed primitive → 직렬화 순서를 그대로 구현한다. Buffer.toJSON 부작용까지 보존하므로 기본 Buffer 숫자 배열은 이 후보에도 남는다. 이를 생략할 때 필요한 예외 판정은 별도로 규칙 수에 포함한다.
