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

- 첫 Java profile은 3초 warmup 조건에서 L1/L2 CPU·allocation·wall 차이를 확보했으나, target CPU의 compiler 비중과 yield 경합이 커 throughput 손실 방향이 뒤집혔다(53609.75 → 64327.75). 정식 21.9% 손실을 이 수치로 대체하지 않는다. 더 긴 9초 warmup으로 동일 scratch L1/L2를 다시 profile한다. 각 process 19초 timeout, 전체 관찰 18초 상한. HEAD/dirty/base/SHA256은 `raw/java-build-fingerprint.json` 그대로이고 receive/reply 및 추가 비용 대조는 `call-shape.md`.

- Java 9초 warmup L2 profile 직전: e8721a0776 clean, base=494c5f4827, scratch jar/SHA256 `raw/java-build-fingerprint.json`, receive/reply와 메시지당 추가 비용 `call-shape.md`. 9초 warmup L1 정상 완료. 최초 9초 시도는 진행 중이던 구 runner의 HTTP timeout 때문에 중단됐으며 `raw/java/warm-L1`에 보존.

- 원인·결정 소유 표·production 수정안을 `report.md`에 먼저 기록했다. 원본 사본 `raw/report-before-implementation.md`. 이제 B 범위의 private codec 최적화와 회귀 검사를 시작한다. public API·spec·binding 변경 없음.

- Java main 최초 회귀 실행은 test 시작 전에 single-use Gradle daemon 종료로 실패했다(`raw/java-regression-before.log`). main 변경 전의 codec과 동일한 scratch 빌드에 새 회귀 test만 복사해 전 상태를 검사한다. production 파일은 아직 수정하지 않았다.

- Node 새 string 경계 test는 전후 모두 4/4 통과(동작은 유지하는 비용 수정). `raw/node-strings-before.log`, `raw/node-strings-after.log`. 최초 codec-after 진단은 formatter의 source mtime이 compile보다 늦어 fresh 판정에서 제외하고, formatted source를 다시 compile한 뒤 확인한다. raw는 보존.

- Java allocation 회귀는 old=256 extra B/header에서 실패, 수정 branch의 ≤96 B/header 상한 통과. 원본 XML `raw/java-regression-before.xml`, `raw/java-regression-after.xml`. 두 언어 전체 format check 통과(`raw/java-format-check.log`, `raw/node-format-check.log`). Java scratch에 production fix만 복사해 bench client/server를 새로 빌드한다.

- Artifact 내용 검사에서 main Node dist가 이전 native parser 후보임을 확인했다. 앞선 185개 통과는 최종 correlation/cache 수정의 검증으로 쓰지 않는다. `--force` 빌드 후 source/compiled signature와 mtime을 검사하고 최종 focused test를 다시 실행한다. scratch도 최종 production source를 복사해 별도로 다시 compile한다.

- Java post-fix L2 profile 직전: main commit ddc13b0615, 테스트 완료한 core JAR(source보다 최신, private stringScratch 확인)을 classpath 첫 항목으로 둔다. fingerprint `raw/java-fixed-core-fingerprint.json`; bench/binding은 최초 fresh scratch 산출물 `raw/java-build-fingerprint.json` 그대로다. receive/reply shape `call-shape.md` 동일. native string 필드별 char[] 대신 reader-local byte scratch, 추가 wait·hop 없음.

- Java post-fix JFR: CanonicalReader.string의 char[] sample source/target 모두 0(전 39,059,456/37,748,736B). 228,958건 count 일치·오류 0, process 전체 16.79초. 관련 permit/lane/multipart/ID/flow helper class parity는 `raw/java-profile-dependency-parity.json`.

- Node 강제 compile 완료. source보다 최신인 dist에서 NON_ASCII_CORRELATION_ID 존재, 기각한 CANONICAL_STRING_SPECIAL/boolean deadline Map 부재, 생성 ID 직접 반환을 확인했다. `raw/node-final-fingerprint.json`. 이 artifact로 codec profile과 최종 185개 focused test를 실행한다.
