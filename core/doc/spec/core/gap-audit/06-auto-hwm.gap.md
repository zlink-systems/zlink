# Auto HWM 스펙-구현 gap 감사

> 감사 도구: codex (정적 코드 대조, 실행 테스트 미실행) · 2026-08-24
> 범위: `core/doc/spec/core/systems/06-auto-hwm.ko.md`와 `core/include/`, `core/src/`, 관련 `core/tests/` 표본

판정: **구현/문서 gap 6건, 요확인 1건**. 코드와 스펙 문서는 수정하지 않았으며, 이 보고서만 작성했다.

## 대조 완료 계약군

- Auto HWM profile 비율·고정 cap·일반/STREAM 경계와 Balanced `recv_ingress` 2 MiB 예외: 일치
- memory input 우선순위, 수동 Core budget의 effective-cap 우회, finite hard-limit 초과 `EINVAL`: 일치
- 공개 ABI: `zlink_ctx_auto_hwm_recalculate`, snapshot ABI v1 상수·구조체·prefix 복사, metrics-reset signature와 `EFAULT`/`ETERM` 경로: 일치
- physical ypipe의 한 번 집계, completion/monitor lane의 application planning 제외, retained-credit owner 이전의 current byte 불변성: 일치
- HWM 축소의 deferred 적용과 retired queue 유지, byte charge의 payload + `sizeof(msg_t)` 계산: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| B. 구현 gap | `06-auto-hwm.ko.md:144-151` — 새 explicit memory limit 또는 수동 Core budget이 기존 수동 reservation과 자동 하한을 수용하지 못하면 setter가 `ENOBUFS`로 실패하고 이전 설정·plan을 유지 | `core/src/runtime/core/ctx_auto_hwm_state.cpp:166-184`, `core/src/runtime/core/ctx_options.cpp:64-95`, `core/src/runtime/core/ctx_auto_hwm_recalc.cpp:97-131` | setter는 detected hard limit만 검사한 뒤 값을 즉시 저장하고 재계산을 예약한다. existing manual reservation 및 auto minimum을 검증하거나 `ENOBUFS`를 반환하는 경로가 없다. 이후 planner는 부족 flag만 설정하므로 이전 설정·plan 보존 계약을 만족하지 못한다. |
| B. 구현 gap | `06-auto-hwm.ko.md:114-119,144-151` — 새 pipe pair는 manual direction의 finite HWM(0이면 역할 상한)을 포함한 방향별 reservation을 확보한 뒤에만 attach를 공개 | `core/src/runtime/core/pipe.cpp:58-67`, `core/src/runtime/core/ctx_physical_queue_registry.cpp:499-563`, `core/src/runtime/core/ctx_physical_queue_registry.cpp:1113-1268` | `create_pipepair_queues`는 전달받은 `first_direction_hwm_`/`second_direction_hwm_`을 reservation 계산에 쓰지 않고 역할별 minimum 두 개만 예약한다. finite manual HWM은 attach 뒤 `plan_application_queues`에서 plan 통계로만 반영되어, 필요한 수동 reservation이 부족해도 attach 전 `ENOBUFS`로 거절되지 않는다. |
| B. 구현 gap | `06-auto-hwm.ko.md:150` — 실행 중 감지한 finite hard limit 감소는 새 입력을 기록하고 budget 부족 flag를 설정 | `core/src/runtime/core/ctx_auto_hwm_state.cpp:119-142,217-220`, `core/src/runtime/core/ctx_auto_hwm_recalc.cpp:97-112` | physical memory와 hard limit은 state 생성 시 한 번만 감지되어 `_input`에 저장되고, 재계산은 그 복사본만 사용한다. 실행 중 hard limit을 다시 감지·기록하는 경로가 없어 감소를 관찰하거나 그에 따른 부족 flag를 만들 수 없다. |
| B. 구현 gap | `06-auto-hwm.ko.md:459-466,536-539` — 최종 크기를 처음에 알 수 없는 multipart는 HWM 도달 시 다음 frame allocation 전에 중단하며, oversize 예외는 admission 시 전체 charge를 아는 complete message 한 건에만 적용 | `core/src/runtime/core/pipe.cpp:825-849`, `core/src/runtime/core/ctx_physical_queue_registry.cpp:814-843`; `core/tests/unittest/unittest_auto_hwm_policy.cpp:463-512` | decoder 경로는 multipart가 비어 있는 queue에서 시작했으면 final frame(`!more`)에도 `allow_empty_exception`/`final_oversize`를 적용한다. 기존 multipart prefix 뒤 final frame이 HWM을 넘는 경우도 수락하며, unit test도 그 수락을 기대한다. 이는 전체 message charge를 최초 admission 시 알 수 없는 multipart에 예외를 확장한다. |
| B. 구현 gap | `06-auto-hwm.ko.md:365-369` — metrics reset은 oversize 누적 count와 largest value를 0으로 초기화 | `core/src/runtime/core/ctx_auto_hwm_recalc.cpp:283-297`, `core/src/runtime/sockets/common/socket_base_monitor.cpp:156-191`, `core/src/runtime/core/pipe.cpp:579-588` | context snapshot의 application oversize 값은 pipe의 `_oversize_message_admission_*`를 socket에서 합산한다. reset은 registry counter와 socket의 admission attempt counter만 0으로 만들며 pipe oversize fields를 reset하지 않는다. 따라서 기존 oversize가 다음 snapshot에도 남는다. |
| D. 구현 서술 낡음 | `06-auto-hwm.ko.md:401-404,432-445,455-461` — decoder는 allocation 전 local provisional charge를 예약하고 enqueue에서 committed로 전환하며, 실패 시 그 reservation을 반환 | `core/src/runtime/core/pipe.cpp:830-889,947-999`, `core/src/runtime/core/ctx_physical_queue_registry.cpp:834-924` | decoder reservation은 queue/generation/frame bytes를 token에 기록할 뿐, application 경로의 `_out_incomplete_bytes`나 registry provisional counter를 allocation 전에 증가시키지 않는다. charge 반영은 `write_reserved_decoder_frame` 이후에 시작하며 release도 token reset만 수행한다. 문서가 설명하는 provisional reservation/전환 구조는 현재 구현과 다르다. |

## 요확인

- `06-auto-hwm.ko.md:199-210`은 `zlink_ctx_auto_hwm_recalculate`의 모든 스레드 호출 안전성을 보장한다. 재계산 자체는 `_opt_sync`와 `_slot_sync`로 직렬화되지만, 공개 API는 raw context handle의 tag를 먼저 역참조한다(`core/src/api/core/context_api.cpp:235-245`). `zlink_ctx_term`은 context를 해제할 수 있으므로(`core/src/api/core/context_api.cpp:117-132`), 다른 thread의 `term`과 이 API가 겹칠 때 lifetime race가 없는지는 정적 대조만으로 확정할 수 없다. concurrent `term`/recalculate stress와 ASan 검증이 필요하다.
