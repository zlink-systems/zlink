Refs #1710

C++의 connect 가능한 일곱 socket에 연결 시도 snapshot을 노출하고 기존 poll 경로에 POLLCONNECT를 투영한다. Endpoint는 소유 문자열이며 세 opaque uint64 token, 닫힌 상태·실패·단계 enum과 진단 정수를 손실 없이 보존한다. 두 native buffer는 완전한 성공까지 필요한 용량으로 늘리고 조회 오류와 protocol 위반을 구분한다. STREAM은 기존 private-using 패턴으로 제외한다.

두 기존 Context 생성자는 공통 초기화 경계에서 필요한 Core export를 참조한다. 구 Core는 link/eager load 또는 lazy Context 초기화에서 거부한다. Core의 물리 연결 선택·재연결·실패 분류와 Framework admission은 변경하지 않는다.

## 검증

- Linux x64 source-built Core5015988d…를 사용한 focused 5/5, 실패·skip 0. 210 enum 조합, 21 malformed 응답 거부, 두 buffer 성장·소유 UTF-8·readiness 독립성·실패 후 회복 이력·일곱 socket/STREAM 표면을 확인했다.
- 잘못된 state 변환과 unknown-kind fallback 두 회귀 주입은 compile/link 후 실제 assertion에서 각각 실패했다. baseline capability와 구 Core Context 허용도 실행으로 확인했다.
- 실제 공개 old Core266ceb96…에 대해 세 build 모드×두 생성자의 link 6건이 필요한 export에서 실패했다. 명시적 lazy consumer 6건은 ELF NOW 없음/JUMP_SLOT과 BEFORE_CONTEXT 후 실패를 확인했으며, 같은 consumer의 eager 6건은 main 전 실패했다. 양성 6건은 실제 Core501 매핑과 Context 생성을 확인했다.
- 원래 old-lazy 이름의 6건은 BIND_NOW였으므로 lazy 증거라는 최초 보고를 철회했다. 원본과 정정 addendum을 보존하고 새 consumer의 실제 조건으로 보완했다.
- PM 검토 뒤 시험 helper의 10ms snapshot 반복을 제거했다. 최초 baseline 후 실제 POLLCONNECT가 있을 때만 조회하고 원래 deadline의 남은 시간으로 기다린다. 영향 target만 재컴파일·단회 CTest 1/1 통과, Core/archive hash 불변 및 실제 child Core501 매핑을 확인했다. 빌드 후처리 wrapper의 CRLF exit2와 실제 cmake/time exit0은 구분했다.
- 첫 readiness fixture 4/5 실패도 보존했다. 부족용량 조회가 readiness를 새로 만드는 것이 아니라 이전 상태를 보존한다는 계약으로 전후 비교를 바로잡았으며 원래 성공 해제·후속 변경 단언은 유지했다.
- 양성 Core는 1.20으로 표시된 미공개 source build다. 공개 1.21 패키지·다른 OS·전체 binding/Framework gate 통과를 주장하지 않는다.

PM이 13파일 전체 diff와 마지막 helper delta, Core/binding 규범, native API/observer/context 경계, 검증 driver 전체 및 실제 raw 로그를 직접 읽었다. 근거: `bindings/doc/spec/README.ko.md:1045-1052`, `core/doc/spec/core/socket/README.ko.md:887-996`, `core/doc/spec/core/05-polling.ko.md:117-121`. 검색어: `connection_attempts_snapshot`, `POLLCONNECT`, `intent_id`, `last_failure_id`, `snapshot`, `readiness`, `Context`.

물리 결과 owner 1→1, binding 투영 및 Context 호환성 판단은 각각 한 곳이다. 새 physical registry/poller/thread/timer/queue/lock/state cache와 message당 비용 추가는 0이다. 공개 API 이름·Core 상수와 기존 오류 계층을 재사용한다. #1710 Framework 적응·공개 패키지 인수는 남아 있어 이 PR로 이슈를 닫지 않는다.

증거: `D:/t/jobs/fx/cx/review1710-cpp-binding-20261029/packet/` 및 `D:/t/jobs/fx/cx/fix1710-cpp-poll-helper-20261029/`.
