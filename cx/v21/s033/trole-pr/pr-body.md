Refs #1639

## 요지

C++ executor가 회전 반환 시점에 idle worker의 수신 역할과 정상 queue head를 한 곳에서 배정합니다(스펙 01-execution/08 §4.4 독점 배정·수신 종료 계약의 적응). 수신 worker 선택 판정 위치는 2→1, 전체 판정 위치는 12→11입니다.

## 바꾼 것

- `offload_executor`: `_roles`·idle 개수·공용 CV를 worker assignment·idle 목록·worker별 CV로 바꿨습니다. 역할 이동은 기존 credit을 유지하고, owner 알림은 게시 callback이 수락한 뒤 지웁니다 (8a8a716b8e)
- `mesh_node_host_service`: host는 한 회전 뒤 반환하고, executor가 수신·게시 stack 종료 뒤 한 번 try-lock해 idle slot과 queue head를 교환합니다 (8a8a716b8e)
- submit 경로의 idle 확인은 목록 전체를 세지 않고 필요한 수(queue 길이+1)에서 멈춥니다 (725ddf3085). 전체 수가 필요한 시험은 friend 접근으로 직접 셉니다 (5a009f8163)
- offload worker OS 스레드에 이름을 붙였습니다. 전에는 이름이 로그에만 쓰여 worker가 만든 쪽 스레드 이름을 물려받았고, 스레드별 CPU 스냅샷에서 찾을 수 없었습니다 (e0ac98360b)
- tooling smoke에 메인 빌드의 source binding 옵션을 넘깁니다 (CMakeLists.txt, verify_tooling_contract.cmake)

## 검증

- 로컬 전체 시험(`pre-pr.sh`, HEAD d790739e78, main 9f0c3677d1 병합): C++ Release 빌드, `ctest -L '^(unit|contract)$'` 303/303 통과, `format.sh --check cpp` 통과. Core 1.25.0, binding 소스 빌드
- 새 시험 T01–T11(`test_cpp_framework_receive_role_handoff.cpp` 등): 각 50/50 통과. 의도한 결함을 넣은 음성 변형마다 검출(T04는 예상된 abort, 나머지 exit 1)
- 기존 handler-entry·executor·TCP mailbox·receive completion·Yield·operation registry 실행 파일 각 50회 통과
- 알려진 T05 same-owner 실패는 이 PR의 pass set에서 뺐고, 후속 same-owner PR에서 고칩니다

## 성능 (BASE = main, PR = 이 브랜치; 1024 B, 배치 B source 0-7·target 8-15, warmup 30초·active 5초, 라운드 내 교대 3회)

| 셀 | BASE msg/s 중앙값 (범위) | PR msg/s 중앙값 (범위) | PR/BASE | CPU µs/msg source/target BASE → PR (중앙값) |
|---|---:|---:|---:|---:|
| send-saturation | 149,743 (120,844–168,118) | 157,393 (127,832–158,781) | 1.051 | 5.29/14.92 → 4.74/11.96 |
| request-backpressure | 47,618 (45,155–49,378) | 46,279 (38,617–47,481) | 0.972 | 59.02/47.69 → 63.49/53.48 |

- 처리량 범위는 두 셀 모두 겹칩니다
- request-backpressure의 CPU는 세 라운드 모두 PR이 높았습니다(target +8%·+17%·+12%). 이 측정에서는 worker 스레드 이름이 없어 idle 확인 경로가 실제로 돌았는지 스레드별로 구분하지 못했습니다. 이 PR의 스레드 이름 수정 뒤 재측정에서 확인합니다
- 원본: `/root/jobs/fx/cx/v21/s033/m1639idle2/raw/measurement-summary.csv`, run별 `measure/attempt4/<round>/<cell>/<variant>/proc-window.json`

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01D9jfewPT9LTVRsk1VFWTdu
