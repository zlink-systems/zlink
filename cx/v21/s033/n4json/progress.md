# Node typed JSON 조사

- 시작 branch perf/4354-node-typed-json, HEAD/base df42a4df4098d543be8dc0b84510cf6d961576d8; tracked/untracked 변경 없음.
- Scratch: ae8a5178b5 git archive. 다른 job worktree/build output을 사용하지 않는다.
- 요청한 lad-node-impl4c/report.md 없음. lad-node-impl4/report.md 및 지정 commit에서 L4 production 호출을 확인했다.
- Node 22.23.3, 모든 명령 taskset -c 6-7,14-15. 성능 판정과 measure.lock 사용 없음.
- 최초 설치: local http-client tarball 없음. 이 worktree에서 registry binding 1.25.0을 내려받고 공식 http-client bootstrap publisher를 실행한다. bootstrap tsc는 quiet guard가 SIGSTOP한 상태다. 강제 재개하지 않는다.
