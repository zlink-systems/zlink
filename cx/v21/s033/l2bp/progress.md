# L2 backpressure 원인 조사 진행

- 작업 branch `perf/4352-l2-bp-reply`, 시작 HEAD `df42a4df4098d543be8dc0b84510cf6d961576d8`, clean.
- 측정 lock을 사용하지 않는다. 모든 실행은 `taskset -c 6-7,14-15`를 상속한다. scratch Java `e8721a0776`, Node `2bef18aa94`. 원본 raw를 삭제하지 않는다.
- 이전 정식 1K quick 결과: Java L1 129815.4 → L2 101436.2 msg/s (`../lad-java-s2/progress.md`); Node L1 28946.2 → L2 20514.8 (`../lad-node-s1/progress.md`). 이 작업의 profile throughput은 잡음이 있으므로 정식 비교 수치로 사용하지 않는다.
- 사전 call-shape: `call-shape.md`. production envelope codec은 해당 ladder commit과 작업 branch에서 동일하다. completion 경로의 await/callback 수는 L1/L2 동일하다.
- scratch 빌드 시작. quiet guard가 build tree를 일시 정지한 상태이며 보호 설정을 바꾸지 않는다. build 로그는 `raw/java-build-initial.log`, `raw/node-pack-bootstrap.log`에 보존한다.

- Node scratch 최초 빌드 성공: `raw/node-build-initial.log`. fresh check 통과. HEAD/dirty/base/sha256: `raw/node-build-fingerprint.json`. 이후 L1/L2 진단 profile은 모두 이 산출물 사용.

- 첫 유효 Node L1 profile: `raw/node/baseline-L1-b/results.json`, CPU/event-loop raw 같은 폴더. 처리량은 진단 값이며 `raw/node-build-fingerprint.json` 산출물과 `call-shape.md` 경로를 사용.
- Node L2 profile 직전: HEAD=2bef18aa94 clean, merge-base=494c5f4827; source/target는 scratch Node만 사용. SHA256은 `raw/node-build-fingerprint.json`. receive=raw `main.js:47-81` vs ladder `:166-205`, reply=raw `:82-86` vs ladder `:206-212`; L2 header encode/decode 외 기다림·Promise·body copy 추가 없음.

- Java scratch 새 빌드 성공: `raw/java-build-initial.log`(31 tasks 실행). source보다 jar가 최신임을 확인. L1 profile 직전 HEAD=e8721a0776 clean, base=494c5f4827, 모든 산출물 scratch. SHA256 `raw/java-build-fingerprint.json`; receive/reply 대조 `call-shape.md`. L1 permit 외 추가 wait/atomic/body copy 없음.

- Node 첫 정상 profile L1/L2 모두 count 일치·오류 0. `raw/node/baseline-L1-b`, `raw/node/baseline-L2`; CPU 요약 `cpu-summary.json`. L1 19444.5 vs L2 13729.0 msg/s(진단 전용), 손실 29.4%. Java 최초 시작 시도는 trigger HTTP가 phase 완료까지 기다리는 계약을 runner가 즉시 응답으로 가정해 중단. 기존 raw를 보존하고 runner를 비동기 HTTP 관찰로 수정.

- Java L2 profile 직전: 최초 scratch 빌드 HEAD=e8721a0776 clean, base=494c5f4827, SHA256 `raw/java-build-fingerprint.json`. receive raw `ZLinkRawBenchServer.java:94-109` vs ladder `:129-185,208-220`, reply raw `:115-127` vs ladder `:240-259`. L2 header/body 소유권 외 wait/poller 추가 없음.
