Refs #2799

raw mesh가 영구 0으로 게시하던 ObservableCounter를 삭제해 `zlink.mesh_node.messages.dropped` 생성을 기존 runtime_metrics/monitoring registry 한 곳으로 모읍니다. 같은 meter의 DoubleCounter와 충돌하던 SDK 경고를 해결합니다. 증가 경로가 없는 atomic 네 개와 전용 callback/배열도 삭제합니다. peer gauge와 전달·요청·terminal 결정은 유지합니다.

## 검증

- 실제 SDK logger로 수정 전 duplicate instrument kind 경고와 exit1을 재현했습니다. 삭제 후 같은 시험은 warning0/exit0입니다.
- 기존 counter API에 세 reason을 각1로 입력한 통제 시험에서 cumulative 두 수집과 raw owner 파괴 뒤에도 각1, 단위·monotonic·정확한 네 label을 확인했습니다. backpressure 양수는 통제 입력이며 실제 native drop 증거로 주장하지 않습니다.
- M6a owner-rejection-observability와 M6b owner-request-rejection은 각각 단회 통과했습니다. 기존 cost/allocation prelude도 실행됐습니다. 미발생 zero series 수 강제만 없애고 payload/reply/terminal·값0·flow/error0 단언은 보존했습니다.
- stock CMake의 관련 3targets 증분 build29단계 성공, focused CTest 등록 확인. 실제 시험은 gdb --return-child-result로 실행했으며 CTest 실행으로 세지 않습니다. -UNDEBUG와 실제 child의 Core266 hash/maps·CPU4/5를 확인했습니다.
- 첫 compile 변환 오류는 보존했고 행동 RED로 세지 않았습니다. 허용6파일 외 source/index/refs/dependencies/native 원본 변경0, diff-check 성공입니다. 공개1.21/전체 Framework gate/원래 ActorSend B1/B2 완료는 주장하지 않습니다.

PM이 전체 6파일 diff, registry/raw/caller, 실행 driver 및 원시 SDK 경고·종료·native·build 로그를 직접 읽었습니다. `messages.dropped|drop|counter|cumulative|누적|delta`로 규범을 대조했습니다. 근거: framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:35-96 및 :180-195. .NET/Java의 단일 counter 소유와 맞춥니다.

분류 B, instrument 생성 owner2→1, raw callback1→0입니다. 새 제품 state/lock/registry/classifier0. 성능1·POSDDD1·불필요코드1 발견을 해당 삭제로 해결했습니다. #2799 실제 stale_target/no_handler 원인 경계 작업은 남습니다.
