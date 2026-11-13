Refs #1639

## 요지

C++ `service_mailbox_t`가 Node·ChannelName owner의 record를 owner별 직렬이 아닌 **독립 claim**으로 꺼냅니다. 스펙 01-execution/02 §2(:97-106): Node·ChannelName handler는 owner queue에서 FIFO로 꺼내지만 gate가 없어 record별 pipeline·handler 진입 순서를 정하지 않습니다. 기존 구현은 같은 Node·Channel의 두 번째 record가 첫 handler가 끝날 때까지 기다려, #4302에서 `DISABLED_`로 뺀 T05(같은 Node owner 진행)가 실패했습니다. 이 PR에서 T05를 다시 켭니다(#4302 리뷰 조건).

## 바꾼 것

- `service_mailbox_t`: 등록된 owner 성질(gate 유무)로 claim과 ready/drain을 정합니다. 독립 owner는 원본 하나를 lock 안에서 옮긴 뒤 다음 ready를 공개하고, 각 claim은 자기 iterator만 정리해 역순 settlement에서도 다른 claim의 수명을 지킵니다. 판정 위치 9→2(직접 claim 허가 2→1, ready/drain 7→1), owner/domain counter 정의 8→2 (985c271c1b)
- `try_claim`: ready 목록 맨 앞의 owner가 지워졌거나 claim할 수 없게 됐으면 건너뜁니다. ready callback이 예외를 내면 lock 밖에서 owner를 되돌려 붙이므로 그런 항목이 앞에 남을 수 있고, 전에는 nullptr로 끝나 같은 domain의 대기 claim이 멈췄습니다 (2288c1c367)
- 기존 시험을 독립 claim 계약에 맞췄습니다. 제품 변경 없음. batch 한 번·직렬 진입을 가정하던 시험은 record별 claim으로 바꿨고, FIFO와 64개 오류 회계 검사는 유지했습니다 (e7fe9fee89, 179528ba0d)
- 여러 worker drain 시험은 진입 순서 대신 record마다 한 번 진입했는지만 봅니다. 단일 worker의 FIFO 단언은 유지합니다(스펙 :99-104). 4-worker에서 1,3,2 진입으로 200회 중 약 절반 실패하던 것이 300/300 통과합니다 (3bcd95957b)
- gate가 있는 Actor·SpotWide·PerActor 경로와 ordinary await·Yield 거부는 바꾸지 않았고, 대조 시험으로 유지를 확인합니다

## 검증

- 로컬 전체 시험(`pre-pr.sh`, HEAD 8b8e955ed8, main c52c06f9ae 병합): C++ Release 빌드, `ctest -L '^(unit|contract)$'` 305/305 통과, `format.sh --check cpp` 통과. Core 1.25.0, binding 소스 빌드. 이후 main(#4302 squash, #4304 .NET) 병합 dbb8fc2b6f는 C++ 파일 차이가 0입니다
- 수정 전 실패: 제품 7개 파일을 기준(3ea1463fd2)으로 되돌리면 새 GTest 14/14 실패, 공개 API 시험 2개(같은 Channel·같은 Node 선진입) 실패
- 새 시험·변형: 각 50/50 통과. ready 유실, 중복 제출, record별 executor wrapper를 주입한 변형은 각 50/50 검출
- 공개 Channel 50회: 같은·다른 Channel, `actor:`로 시작하는 ChannelName 모두 B 선진입 20/20. 공개 gated Actor 50회: 선진입 0/20(직렬 유지)
- T05 `ReceiveRoleHandoff.T05SameNodeOwnerProgress`: `DISABLED_` 없이 50/50 통과
- 범위: Linux Release C++입니다. 성능은 이 PR에서 재지 않았고, #1639 측정에서 함께 봅니다

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01D9jfewPT9LTVRsk1VFWTdu
