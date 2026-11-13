# C++ typed JSON 조사

- 기준 HEAD=df42a4df4098d543be8dc0b84510cf6d961576d8; branch perf/4354-cpp-typed-json; scratch 62e766f88d git archive; production codec 두 파일은 같은 SHA256(raw/codec-baseline-identity.json).
- Profile 조건: CPUs 6-7,14-15, run ≤20s, measure.lock 미사용. Profile call-shape는 call-shape.md에 기록. Throughput은 판정하지 않는다.
- Release/source binding/Core 1.25.0/-j3/25 GiB 제한. Build wrapper 뒤에서 taskset을 다시 적용해 wrapper의 0-11 설정을 제한한다. dependency install은 끄고 기존 package를 읽는다.

- 첫 계수: production JSON bulk encode/decode C++ new는 1K 5/32회, 4K 7/38회. raw/baseline-bulk-allocations.txt. Registry의 string owner 및 Core metadata는 이 분리 probe 밖이므로 전체 default serializer probe에서 별도 계수한다. 처리량 판정 없음.
