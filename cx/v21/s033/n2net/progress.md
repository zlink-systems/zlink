# 진행 상태

- 시작: branch perf/4352-node-l2-net, HEAD 5f3a59bff64a7044373f9625d2e6041a130c26b4, clean. 사용자가 internal-docs, GitHub, push, PR, 다른 worktree, measure.lock, sub-agent 사용을 금지함.
- 지정 문서와 call-shape, Node·.NET codec 읽음. 수정 전 비교표·소유 표·제안 report.md 작성. dependency 설치는 이 작업의 tools/deps에 수행, 다른 worktree 산출물 사용 없음.
- 첫 build는 measurement hold 때문에 SIGSTOP 상태. 보호 설정을 변경하지 않고 대기함.

- build/검증 fingerprint: before 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/before-fingerprint.json (source/output fresh, own worktree paths·sha256).

- 첫 warm-before-L1은 source startup deadline 9s 초과로 profile 미완료. raw 보존. 측정 구간/timeout은 그대로 두고 source/target 초기화를 동시에 시작해 startup 대기를 겹치며, profile 파일 저장 종료 대기는 3s로 둠(각 child의 hard timeout 19s+kill-after1s는 유지). 이는 throughput 셀이 아닌 CPU shape 진단 준비이며 전후 같은 runner를 사용함.

- build/검증 fingerprint: warm-before-L1-b 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/warm-before-L1-b-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile warm-before-L1-b L1 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- warm-before-L1-b source는 protobuf package의 server bundle 미생성으로 종료함. tsc build와 별도로 공식 esbuild bundle 명령으로 이 worktree에 생성. raw 보존. allocation sampling 4096B, 종료는 hard timeout(20s 최대)까지 파일 저장을 허용함. 유효 전후 profile에는 모두 이 설정을 사용함.

- build/검증 fingerprint: warm-before-L1-c 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/warm-before-L1-c-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile warm-before-L1-c L1 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: before-L1 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/before-L1-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile before-L1 L1 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: warm-before-L2 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/warm-before-L2-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile warm-before-L2 L2 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- 첫 allocation 수치: request encode 21.101 allocations / 903.50 B per message (2000 calls, --jitless exact V8 tracking, raw/alloc-before/allocation-summary.json). Buffer runtime 내부 allocation 포함, native backing bytes와 별도.

- Profile runner 준비 오류 추가 분리: trigger가 accepted를 먼저 반환해 idle→warmup 전 순간에 active를 시작하던 race(409)를 수정함. phase가 실제 관측된 뒤 idle을 기다림. timeout wrapper가 SIGTERM을 받으면 kill-after=1을 즉시 예약하므로 이제 Node child만 종료 신호를 받아 profile 저장 시간을 확보함(hard 19s+1s 유지). sampling 32KiB로 loader trace 크기와 측정 간섭을 줄임. 기존 incomplete/preliminary profile을 전후 결과에 쓰지 않음.

- build/검증 fingerprint: warm-before-L1-final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/warm-before-L1-final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile warm-before-L1-final L1 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: before-L1-final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/before-L1-final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile before-L1-final L1 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: warm-before-L2-final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/warm-before-L2-final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile warm-before-L2-final L2 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: before-L2-final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/before-L2-final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile before-L2-final L2 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- 유효 수정 전 profile: raw/node/before-L1-final, before-L2-final; 각 앞 discarded warm run 보존. source/target exit0, lifecycle<12s. CPU raw/cpu-summary.json, per-op allocations raw/alloc-before/allocation-summary.json에 기록.

- focused 검사 중 message-flow 파일이 stream-connector dist/package.json 미생성 때문에 CommonJS exports를 ESM으로 해석해 import 단계 실패. 공식 prepare_browser_build.mjs가 쓰는 동일한 own dist/package.json을 생성함. 구현/expectation 수정 없음. 이 파일만 재실행할 예정.

- build/검증 fingerprint: warm-after-L1-final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/warm-after-L1-final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile warm-after-L1-final L1 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: after-L1-final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/after-L1-final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile after-L1-final L1 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: warm-after-L2-final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/warm-after-L2-final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile warm-after-L2-final L2 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: after-L2-final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/after-L2-final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile after-L2-final L2 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: final 5f3a59bff64a7044373f9625d2e6041a130c26b4 dirty=True merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/final-fingerprint.json (source/output fresh, own worktree paths·sha256).

- 최종 검증: 13 files 273 tests 통과, 실패/skip0. format(JS 포함), lint, typecheck, git diff --check 통과. Baseline 최종 allocation budget 실패(7.002>4.25), final 통과(4/136B). Profile 4 runs count 일치/errors0/CPU·policy·20s cap audit 통과. Source envelope17.400→15.149%, target7.225→9.603%; throughput 개선 판정 안 함. 최종 optimized request10.101/487.5B→5.101/271.5B. report.md 완료, raw 보존.
