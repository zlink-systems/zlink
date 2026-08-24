# 스펙-구현 gap 통합 검토 문서

> 각 문서의 감사 보고서(`<문서>.gap.md`)에서 발견한 gap을 한 곳에 모아 항목별로 검토한다.
> 상세 근거(스펙·코드 파일:라인)는 각 감사 보고서를 따른다. 스펙에는 아직 반영하지 않았다.
>
> **처분 유형**: `스펙수정`(문서가 낡음 — 코드에 맞춤) · `코드수정`(스펙이 목표 — 구현 미달)
> · `잔재의심`(제거·미완성 기능의 코드 잔재 — 코드 제거/완성 판단) · `계약결정`(설계 판단 필요)
> · `test추가`(코드·스펙 일치, 검증만 부재)
>
> ✅ = 이미 스펙 반영 완료(앞선 "누락 채우기" 단계 — 재검토 가능) / ☐ = 미처리, 검토 대기

## 확정 코드 버그 (우선 처리 권장)

| 검토 | 항목 | 제안 방향 |
|---|---|---|
| ☐ | **proxy_steerable PAUSE/RESUME 반전** — `proxy.cpp:183-188`. memcmp 조건이 반전돼 있고 비교 문자열도 `"\x05PAUSE"`(length-prefix 잔재)로 잘못됨. PAUSE→active, RESUME→paused로 동작 | **코드수정** — `msiz == 5 && 0 == memcmp(command, "PAUSE", 5)` → `state = paused`, RESUME → `state = active`. 스펙(PAUSE=pause)이 명백한 의도 |

## 잔재 의심 — framework 사용 여부로 판별 완료, 사용자 결정 반영 (2026-08-24)

> **사용자 결정**: `ZLINK_STREAM_OPT_NOTIFY`는 동작해야 하는 옵션 → 구현 완성. 나머지는 모두 제거 대상.

> 판별 기준(사용자 지정): **framework**(`/home/hep7/project/zlink/framework/` 실소스)가 binding
> API를 통해 사용하면 필요 기능, 사용하지 않으면 잔재 확률 높음. bindings는 C ABI를 기계적으로
> 전부 매핑하므로 binding 존재는 증거가 아니다. (framework cpp는 수신 로직이 실재함을 대조군으로 확인)

| 검토 | 항목 | framework 사용 | 제안 방향 |
|---|---|---|---|
| ✅제거 | **retained lease 기능 전체** — C API 6종(`*_with_hwm_budget_lease` 4변형 + 구형 `zlink_recv_with_hwm_budget_lease` + `zlink_hwm_budget_lease_release`), binding 표면 `receive_retained()` (cpp·python·node·go·rust) | **0건** — framework 전 언어(cpp·java·dotnet·node·python)에서 receive_retained/lease 사용 없음 (사용자 기억: 제거 대상) | **제거 대상 유력** — 단 제거 범위가 큼: ① core C API·구현·`test_retained_hwm_credit` ② bindings의 receive_retained 표면 5개 언어 ③ 06-auto-hwm 스펙의 retained-credit lease 계약, glossary 항목 ④ budget snapshot의 lease 전제 field들(`application_accounted_bytes`, `outstanding_application_lease_count`, `deferred_origin_credit_bytes`, `retired_queue_count` 등)과 accounting 규칙 — 별도 제거 계획 필요 |
| ✅제거 | **`zlink_proxy_steerable`** (+ PAUSE/RESUME 반전 버그) | **0건** — bindings(python·node)가 표면 노출하나 framework 미사용 | **제거 검토** — 제거하면 PAUSE/RESUME 버그도 소멸. 존치(외부 앱 개발자용 유틸)로 결정 시 버그 수정 필수 |
| ✅제거 | **`ZLINK_SOCKET_ANY` "filter API"** | **0건** (c/perf 내부 도구의 default 인자 사용뿐) | **스펙 문구 수정** — "filter API 전용" 서술 제거, 예약 wildcard로 표기. filter API는 잔재 확정 |
| ✅제거 | **discovery route value size option** | **0건** (bindings 코드표 미러뿐) | **잔재 유력** → enum·매핑·스펙 제거 검토 (discovery 잔재) |
| ✅제거 | **prefix legacy int 경로** (`zlink_ctx_set(..., int)` + `atoi`) | **0건** — framework은 THREAD_NAME_PREFIX 자체를 미사용, bindings도 문자열 경로만 | **잔재 유력** → legacy int 경로 제거 + 스펙 legacy 표기 제거 |
| ✅구현 | **`ZLINK_STREAM_OPT_NOTIFY`** — set/get만 존재, 0-byte 알림 record 생성 경로 미구현 (08-stream 감사 #2) | **0건** | **잔재/미구현 판정 필요** — 미구현 기능이면 코드 완성, 취소된 설계면 옵션·스펙 제거 |
| ✅제거 | **DEALER receive의 `REPLY(2)`/`ERROR_REPLY(3)` record kind** — 반환 경로 없음, reply envelope는 `EPROTO` 폐기 (06-dealer 감사 #3) | **0건** | **잔재 유력** — enum 값·스펙의 record 종류 서술 축소 검토 (reply는 completion callback 전용이 현 구조) |

## 01-context (감사: 9건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | B | `zlink_ctx_new` 실패 시 errno 미설정 (`context_api.cpp:101-115`) | **코드수정** — 두 실패 경로에 `ENOMEM` 등 errno 설정. 1줄 수준 |
| ✅ | C | BLOCKY 실제 효과 = 이후 생성 socket 기본 LINGER=0 (term 자체는 무관) | **스펙수정 완료** — zmq blocky 의미와 일치, 반영 유지 권장 |
| ✅ | C | AUTO_HWM_ENABLE "즉시 반영" → 실제 debounce 3000ms 예약 | **스펙수정 완료** — 유지 권장 |
| ☐ | B | memory limit·Core budget 수용 불가 시 `ENOBUFS` 계약 vs 실제 EINVAL/저장 후 flag | **계약결정** — 06-auto-hwm #1·#2와 동일 계열. reservation 검증이 미구현 기능인지(코드수정) 설계 변경인지(스펙수정) 판단 필요 |
| ☐ | C | thread-name prefix NUL 검증 없음 (임의 1..16 byte 수용) | **코드수정(경량)** — 계약대로 NUL 검증 추가가 자연스러움. 또는 스펙 완화 |
| ✅ | C | `zlink_ctx_get_data` null output → EINVAL (EFAULT 아님) | **스펙수정 완료** — 03-errors 규약(INVALID_ARGUMENT→EINVAL)과 일치, 유지 권장 |
| ✅ | A | `error_out_` NULL 허용 | **스펙수정 완료** — 유지 권장 |
| ✅ | C | IO_THREADS/MAX_SOCKETS 설정은 성공하나 runtime pool은 첫 socket 생성 시 고정 | **스펙수정 완료** — 유지 권장 |
| ☐ | 요확인 | term 동시 호출 lifetime race | **test추가** — 동시 term/get/shutdown stress + ASan |

## 02-message (감사: 6건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ✅ | A | null handle·invalid message의 함수별 결과(EFAULT/NULL/0/-1) | **스펙수정 완료** — 유지 권장 |
| ☐ | B | 미초기화 storage의 type byte 우연 일치 시 `data()`가 쓰레기 pointer 반환 가능 | **코드수정** — `check()`가 우연 통과하지 않게 판별 강화. 스펙의 "미초기화→NULL" 계약 유지 |
| ✅ | A | `zlink_msg_refcnt`의 `error_out_` NULL 허용 | **스펙수정 완료** — 유지 권장 |
| ☐ | C | refcnt read가 fallback build(mutex/volatile)에서 비atomic | **코드수정(경량)** — fallback `get()`에 lock 추가. 또는 스펙에 build 조건 명시 |
| ✅ | C | multipart send 실패 시 나머지 part 소비(빈 초기화) — "소비 않음" 서술 정정 | **스펙수정 완료** — PUB 계약과 일관, 유지 권장 |
| ✅ | D | receive helper의 source 검증 없음(저장 후 반환) | **스펙수정 완료** (내부 서술) — 유지. 단 source 검증이 의도였다면 코드수정으로 전환 |
| ☐ | 요확인 | metadata 매크로의 codec 미연결 의심 | **잔재의심/test추가** — wire-level test로 상수 연결 확인 |

## 03-errors (감사: 9건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | B | `zlink_set/get_option` descriptor 미발견 시 errno 미설정 | **코드수정(경량)** — `EINVAL` 설정 추가 |
| ✅ | A | fallback errno 18개(`+1..+18`) 미문서화 | **스펙수정 완료** — 유지 권장 |
| ✅ | C | version 0.12.0 → 0.13.0 | **스펙수정 완료** — 객관 사실 |
| ✅ | C | submit 대응표 누락 errno(EHOSTUNREACH·ECONNREFUSED·EMTHREAD, ESHUTDOWN→TERMINATED) | **스펙수정 완료** — 정규화 helper 기준. 대응표가 의도와 다르면 재검토 |
| ✅ | C | request 대응표 누락(EHOSTUNREACH·EFAULT) | **스펙수정 완료** — 상동 |
| ✅ | C | config 표의 EINVAL 중복(INVALID_STATE 행) 제거 | **스펙수정 완료** — 유지 권장 |
| ✅ | C | strerror 수명(일부 libc storage) 현실화 | **스펙수정 완료** — 유지 권장. library-owned 보장이 목표면 코드수정으로 전환 |
| ✅ | D | "0-706 전역 고유"·"raw errno callback"·"13개 값" 낡음 | **스펙수정 완료** — 내부 서술, 유지 |
| ☐ | 요확인 | `zlink_version` null 역참조 / strerror thread-safety 전제 미정의 | **계약결정** — null 허용 여부·전제조건을 계약으로 확정 후 코드 또는 스펙 반영 |

## socket/README (감사: 14건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | B | `ZLINK_SOCKET_ANY` filter API 부재 | → 상단 "잔재 의심" 참조 |
| ✅ | A | `SEND_PENDING_MAX_MSGS/BYTES`(0x303A/B) 옵션 미문서화 | **스펙수정 완료** — 유지 권장 |
| ✅ | C | `ZMP_METADATA`는 binary가 아니라 int 0/1 | **스펙수정 완료** — 유지 권장 |
| ☐ | C | 제거 전 discovery route value size option 조회 미지원 | → 상단 "잔재 의심" 참조 |
| ☐ | C | `OPT_BLOCKY` socket 경로가 ENOTSUP 아닌 EINVAL | **코드수정(경량)** — owner 매핑 고쳐 NOT_SUPPORTED 반환이 자연스러움. 또는 스펙수정 |
| ☐ | C | retry 예산 소진 시 EAGAIN이 아니라 마지막 원인(NOT_CONNECTED 등) 복원 | **계약결정** — "소진=재시도 가능(EAGAIN)" vs "원인 보존" 중 택일 후 한쪽 수정 |
| ✅ | C | `terminal_errno`가 TERMINAL 한정 아님(TIMED_OUT=ETIMEDOUT 포함) | **스펙수정 완료** — 유지 권장 |
| ✅ | C | raw routing ID의 STREAM 예외(EINVAL 거절) 미기술 | **스펙수정 완료** — 유지 권장 |
| ☐ | C | TLS setter가 type/role 미검사 (스펙: 미지원 type ENOTSUP) | **코드수정** — setter에 type 검증 추가가 자연스러움. 또는 스펙 완화 |
| ✅ | A | async options 공개 규칙(struct_size 필수, timeout 0=무기한, op_id_out 선택) | **스펙수정 완료** — 유지 권장 |
| ✅ | A | STREAM routed target 지원·non-NULL 필수 | **스펙수정 완료** — 유지 권장 |
| ✅ | A | STREAM callback self-close = EBUSY 예외 | **스펙수정 완료** — 유지 권장 |
| ✅ | A | monitor open의 `monitor_hwm_bytes` 입력 | **스펙수정 완료** — 유지 권장 |
| ✅ | D | monitor status ABI v3 → v4 | **스펙수정 완료** — 객관 사실 |
| ☐ | 요확인 | handle 해제 직전 동시 API 진입 pin 여부 | **test추가** — close/send/option/monitor 교차 stress + ASan/TSan |

## systems/06-auto-hwm (감사: 6건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | B | setter의 reservation 검증·ENOBUFS 경로 부재 (실제: 저장 후 INSUFFICIENT flag) | **계약결정** — 미완성 기능(코드수정)인지 설계 단순화(스펙수정)인지. 01-context #4와 함께 판정 |
| ☐ | B | pipe pair attach가 manual HWM reservation을 검증하지 않음 | **계약결정** — 상동 계열 |
| ☐ | B | hard limit 실행 중 재감지 없음 (시작 시 1회) | **계약결정** — 재감지가 목표 기능인지. 목표면 코드수정, 아니면 스펙 문구 축소 |
| ☐ | B | oversize 예외가 빈 queue에서 시작한 multipart final frame에도 적용 (unit test가 이 동작 기대) | **계약결정→스펙수정 유력** — test가 현 동작을 명시적으로 기대하므로 의도된 확장일 가능성 높음. 확인 후 스펙에 반영 |
| ☐ | B | metrics reset이 pipe 단위 oversize 누적을 초기화하지 않음 | **코드수정(경량)** — reset 경로에 pipe field 포함이 자연스러움. 또는 스펙에서 oversize 제외 |
| ✅ | D | decoder reservation 내부 서술(token 기록, write 시점 반영) | **스펙수정 완료** — 내부 서술, 유지 |
| ☐ | 요확인 | recalculate vs 동시 term race | **test추가** — 01-context 요확인과 동일 계열 |

## 06-monitoring (감사: 3건) — 전부 롤백됨, 미처리

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | B | mode 상호 배타가 비대칭 (handler→recv만 EBUSY, recv→handler 허용) | **계약결정** — 대칭 배타가 의도면 코드수정(recv mode 기록 추가), 아니면 스펙수정 |
| ☐ | B | queue 포화 시 aggregate·우선 보존·계수 없음 (무조건 drop) | **계약결정** — aggregate가 미구현 기능인지(코드수정, 규모 큼) 낡은 설계인지(스펙수정). event version 3 고정과 관련 |
| ☐ | B | status 호출의 단일 snapshot 경계 미보장 (field 군별 시점 상이) | **계약결정** — 단일 경계가 목표면 코드수정(lock 통합), 진단 용도로 충분하면 스펙수정 |
| ☐ | 요확인 | single consumer가 호출자 의무인지 Core 보장인지 미확정 | **계약결정** — 의무로 명시(스펙수정)가 현실적 |

## 05-polling (감사: 11건) — 전부 롤백됨, 미처리

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | C | `zlink_poll` timeout 반환: 스펙 0 vs 실제 -1/EAGAIN, item 0개는 즉시 0 | **계약결정** — poll 계열 관례는 timeout=0. 코드수정(0 정규화) 권장, 단 기존 사용자 영향 확인 |
| ☐ | C | POLLCOMPLETION 허용 socket이 PAIR·STREAM까지 넓음 | **스펙수정 유력** — 구현·async 표면과 정합. 축소가 의도면 코드수정 |
| ☐ | C | completion 처리 wait가 0이 아니라 POLLCOMPLETION event 반환 (test 기대) | **스펙수정 유력** — integration test가 event 반환을 기대 |
| ☐ | B | completion bit 오용 시 EINVAL 검증 부재 | **코드수정(경량)** — 스펙의 검증 계약 구현 |
| ☐ | B | event mask·source별 지원 검증 부재 (ENOTSUP 경로 없음) | **코드수정** — add/modify에 mask 검증 추가. 또는 스펙 완화 |
| ☐ | C | 중복 add: 스펙 EEXIST/CONFLICT vs 실제 EINVAL | **계약결정** — CONFLICT(707)가 이 용도로 추가된 enum이면 코드수정이 맞음 |
| ☐ | C | 없는 source modify/remove: 스펙 ENOENT/NOT_FOUND vs 실제 EINVAL | **계약결정** — NOT_FOUND(706) 취지상 코드수정이 맞아 보임 |
| ☐ | C | TIMER event의 fd field에 내부 signaler FD 기록 | **코드수정(경량)** — 스펙대로 TIMER에서 fd 미기록(0)이 깔끔. 또는 스펙수정 |
| ☐ | A | poller 함수 세부(new NULL/ENOMEM, destroy nulling, size, wait EINVAL, error_out NULL 허용) 미문서화 | **스펙수정** — 관찰 동작 문서화 (안전한 A류) |
| ☐ | A | FD의 POLLPRI 변환·오류 bit→POLLERR 의미 미문서화 | **스펙수정** — 상동 |
| ☐ | C | source 등록이 lifetime pin 획득 (close 선행 허용, contract test가 이 순서 사용) | **스펙수정 유력** — test가 pin 동작 기대 |
| ☐ | 요확인 | callback 중 같은 poller 호출 허용 범위 미정의 | **계약결정** — 허용 조합을 스펙에 명시 |

## 07-utilities (감사: 7건) — 전부 롤백됨, 미처리

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | C | atomic_counter_new/stopwatch_start/thread_start 할당 실패 = abort (NULL 반환 경로 없음) | **계약결정** — 유틸리티의 abort-on-OOM은 zmq 관례. 관례 수용이면 스펙수정, NULL 계약 유지면 코드수정 |
| ☐ | C | timer fire count가 누적이 아니라 start마다 1부터 | **계약결정** — 어느 쪽이 의도인지. 실사용 영향 작음 → 스펙수정 무난 |
| ☐ | A | `interval_ns == 0` → EINVAL 미문서화 | **스펙수정** — 안전한 A류 |
| ☐ | B | proxy PAUSE/RESUME 반전 | → 상단 "확정 코드 버그" |
| ☐ | A | STATISTICS 8-frame uint64 reply layout 미문서화 | **스펙수정** — 안전한 A류 (코드 주석에 이미 명세 존재) |
| ☐ | 요확인 | frontend==backend 동일 handle + control 조합의 NULL 역참조 가능성 | **코드수정(경량)** — 진입 시 동일 handle 거절이 안전 |
| ☐ | 요확인 | topic std::string 복사 OOM의 C API 변환 여부 | **test추가** — OOM 주입 확인 |

## socket/01-pair (감사: 4건) — 코드·스펙 일치, test만 부재

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | B×4 | 양방향 send/recv·NULL RID·record 원자성·async 표면·flow state의 PAIR 전용 contract test 부재 | **test추가** — 4개 test 신설 (스펙·코드 수정 불필요) |

## socket/02-pub (감사: 5건) — 롤백됨, 미처리

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | C | NODROP=1의 BACKPRESSURED가 무조건 아님 (blocking 호출은 대기 후 성공 가능) | **스펙수정 유력** — send timeout 의미와 정합. 무조건 즉시 반환이 의도면 코드수정 |
| ☐ | B | 검증 단계 실패(topic/flag 변경 등)는 sequence를 닫지 않음 | **계약결정** — "모든 실패=닫음"이 의도면 코드수정(prepare 실패에 abort 추가), 아니면 스펙 세분화 |
| ☐ | C | 0/1 옵션이 실제로는 음수만 거절 (2+는 on 정규화) | **스펙수정 무난** — 또는 setter에 0/1 검증 추가 |
| ☐ | C | MANUAL_LAST_VALUE는 cache가 아니라 manual+마지막 구독 pipe 전달 | **스펙수정** — 이름과 동작 괴리는 별도 논의 |
| ☐ | A | APPROVE/REJECT_SUBSCRIBE는 쓰기 전용(조회 EINVAL)·manual 전용 | **스펙수정** — 안전한 A류 |
| ☐ | 요확인 | topic 복사 OOM 경로 | → 07-utilities 요확인과 동일 계열 |

## socket/03-sub (감사: 6건) — 롤백됨, 미처리

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | C | subscription_at 작은 buffer: 스펙 BUFFER_TOO_SMALL/ENOBUFS vs 실제 EINVAL | **계약결정** — BUFFER_TOO_SMALL(707대)가 이 용도의 enum이면 코드수정이 맞음 (05-xsub 동일) |
| ☐ | C | 미지원 handle type: 스펙 ENOTSUP vs 실제 EINVAL | **계약결정** — 상동 |
| ☐ | A | index가 등록순이 아니라 정렬된 snapshot 순서 | **스펙수정** — 안전한 A류 |
| ☐ | A | filter_out_에 NUL 미기록 (C 문자열 아님) | **스펙수정** — 안전한 A류 (binding 구현에 중요) |
| ☐ | A | is_pattern_out_ NULL 허용 | **스펙수정** — 안전한 A류 |
| ☐ | A | `zlink_subscribe_part_with_hwm_budget_lease` 미문서화 | → 상단 "잔재 의심" (사용자: 제거 기능 잔재 가능성) |

## socket/04-xpub (감사: 6건) — 롤백됨, 미처리

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | A | 옵션 기본값(전부 0, WELCOME_MSG 빈 값=미전송, TOPICS_COUNT 초기 0) 미문서화 | **스펙수정** — 안전한 A류 |
| ☐ | C | 0/1 옵션 실제 수용 범위 | → 02-pub 동일 항목과 함께 |
| ☐ | A | MANUAL_LAST_VALUE의 manual 전이·last-pipe 효과 | → 02-pub 동일 항목과 함께 |
| ☐ | B | xpub_recv_part의 source_rid가 socket별 아닌 thread-local (다른 XPUB 호출 시 덮임) | **계약결정** — socket별 유효가 의도면 코드수정(socket별 storage), 아니면 스펙수정 |
| ☐ | A | buffer 부족 시 event 이미 소비(재시도 불가)·필요 길이 기록·source_rid NULL 허용 | **스펙수정** — 재시도 불가는 사용자에게 중요한 계약. 소비 전 검사(코드수정)도 검토 가치 |
| ☐ | B | xpub_recv_part·publish_part 직접 contract test 부재 | **test추가** |
| ☐ | 요확인 | 동시 구독 이벤트 중 approve/reject 대상(_last_pipe) 관찰창 | **계약결정** — 대상 규칙을 스펙에 명시 |

## 08-runtime-boundary (감사: 1건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ✅ | D | Core 0.9.0 → 0.13.0 (07-layout·02-threading 포함) | **스펙수정 완료** — 객관 사실 |

## socket/05-xsub (감사: 8건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | C | XSUB 자체는 filter-match 수신을 하지 않음 (`options.filter=false` — 들어온 message 전부 전달, filtering은 upstream XPUB 몫) | **스펙수정 유력** — zmq 표준 XSUB 의미와 일치. XSUB가 자체 필터링해야 한다는 설계면 코드수정 |
| ☐ | C | subscription_at 오류: BUFFER_TOO_SMALL/ENOBUFS·ENOTSUP vs 실제 EINVAL | **계약결정** — 03-sub 동일 항목과 함께 (전용 result 값이 있으므로 코드수정이 자연스러움) |
| ☐ | A | index = lexicographic 정렬 snapshot 순서 | **스펙수정** — 03-sub과 함께 |
| ☐ | A | filter_out_ NUL 미기록 | **스펙수정** — 03-sub과 함께 |
| ☐ | A | is_pattern_out_ NULL 허용 | **스펙수정** — 03-sub과 함께 |
| ☐ | A | subscribe lease 변형 미문서화 | → 상단 "잔재 의심" (lease 전체 제거 대상 유력) |
| ☐ | A | 중복 구독 refcount·마지막 해제만 upstream 전송·미등록 해제 미전송 | **스펙수정** — zmq 표준 동작, 관찰 가능한 계약 |
| ☐ | B | XSUB 공개 helper 직접 contract test 부재 | **test추가** |

## socket/06-dealer (감사: 5건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | C | PROBE 허용값 0/1 vs 실제 음수만 거절(2+=on) | **계약결정(공통)** — pub/xpub/router 0/1 옵션 전체와 한 번에: 스펙수정(0=off·양수=on) 또는 setter 검증 추가 |
| ☐ | C | PROBE get이 exact size 요구·`*optvallen_` 미갱신 | **계약결정** — get 규약을 스펙에 맞출지(코드수정) 실제로 좁힐지(스펙수정) |
| ☐ | C | receive가 REPLY/ERROR_REPLY record를 반환하지 않음 (EPROTO 폐기) | → 상단 "잔재 의심" (dead enum) |
| ☐ | B | pair 불일치 flow frame을 event 없이 폐기 (STALE 보고 안 함) | **계약결정** — router #4와 동일: silent drop을 계약으로 인정(스펙수정)할지 STALE 보고 추가(코드수정)할지 |
| ☐ | B | completion pipe 부재·write 실패가 BACKPRESSURED로 노출 | **계약결정** — router #3와 동일: 연결 소멸을 NOT_CONNECTED로 구분(코드수정)할지 문서를 완화할지 |

## socket/07-router (감사: 4건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | A | router lease 변형 미문서화 | → 상단 "잔재 의심" |
| ☐ | C | MANDATORY/PROBE 0/1 vs 음수만 거절 | → 06-dealer #1과 공통 처리 |
| ☐ | B | completion lane 비-backpressure 보장 vs EAGAIN→BACKPRESSURED 노출 | → 06-dealer #5와 공통 |
| ☐ | B | pair 불일치 flow frame silent drop | → 06-dealer #4와 공통 |

## socket/08-stream (감사: 9건, 요확인 1)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | A | NOTIFY 기본값 0 미기술 | NOTIFY 존치 결정 시 **스펙수정** (제거면 무의미) |
| ☐ | B | NOTIFY 알림 record 미구현 | → 상단 "잔재 의심" |
| ☐ | B | 첫 recv_part가 raw-part 모드를 고정하지 않음 (이후 handler 등록 성공) | **계약결정** — 모드 고정이 의도면 코드수정, 아니면 스펙수정 |
| ☐ | A | 길이 0 part 송신 = peer 종료 요청 (미문서화) | **스펙수정** — 중요한 관찰 동작 |
| ☐ | B | 정확히 6-byte 빈 packet(0/0)이 다음 byte 도착 전까지 callback 안 됨 (test는 즉시 기대) | **코드수정 유력** — parser 경계 처리, test 기대와 불일치 |
| ☐ | C | maxmsgsize가 header+body 합계도 거부 (문서는 각각만) | **스펙수정** — 합계 검사가 더 안전한 실제 동작 |
| ☐ | D | WS/WSS gather write 미지원 서술 ↔ 실제 지원(`async_writev`) | **스펙수정** — 기존 원문 상충 서술의 정답 확정 |
| ☐ | D | STREAM/TCP speculative write "상시 on·env 비제어" ↔ `ZLINK_ASIO_STREAM_ASYNC_WRITE`로 off 가능 | **스펙수정** |
| ☐ | A | `ZLINK_STREAM_PIPE_LWM_HINT` env(기본 4, ×1024 byte) 미문서화 | **스펙수정** |
| ☐ | 요확인 | 처리량 수치(1493/696/382 MB/s)는 벤치 재현 필요 | **test추가/보류** — 재현 전까지 수치에 측정 조건 명시 |

## protocol/01-zmp (감사: 8건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | A | decoder의 flag 거부 규칙(RESERVED≠0, bit 5-7, 금지 조합) 미문서화 | **스펙수정** — 상호운용 wire 계약, 반드시 문서화 |
| ☐ | A | READY metadata byte 배치(`[name u8][name][value u32 BE][value]`) + `Zlink-Max-Message-Size` 항상 추가 미문서화 | **스펙수정** — 상호운용 필수 |
| ☐ | A | ERROR control type `0x05` + body 배치 미문서화 | **스펙수정** — 상호운용 필수 |
| ☐ | B | encoder가 payload 길이를 상한 검사 없이 u32로 절단 (4GiB 초과 시 header/body 불일치) | **코드수정** — 상한 검사 후 거부. 실버그 |
| ☐ | B | error reply의 errno=0이 성공 completion으로 둔갑 가능 | **계약결정→코드수정 유력** — decoder에서 0 거부 또는 송신측 의무 명시 |
| ☐ | C | passive 쪽은 HELLO만 먼저 보내고 READY는 peer READY 후 (diagram의 "한 buffer HELLO+READY"와 다름) | **스펙수정** — handshake diagram 정정 |
| ☐ | C | metadata의 Routing-Id는 DEALER/ROUTER READY에만 추가 | **스펙수정** |
| ☐ | C | reply diagram의 routing_id는 wire part가 아니라 local 선택 key | **스펙수정** — diagram 주석 정정 |

## protocol/02-raw — **gap 0건 (검증 항목 14개 전부 일치)** ✅

## 04-events (감사: 5건) — 구현·스펙 일치, 전부 test 부재

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | B×5 | no-op frame 무발생 / RESUMED routing ID·STALE epoch 대응 / WRITABLE flag 부재 케이스 / mask bit 제외 검증 / commit 순서 보존 — 공개 표면 contract test 부재 | **test추가** — 5개 test 신설 |

## systems/05-connection-memory (감사: 2건)

| 검토 | 분류 | 항목 | 제안 방향 |
|---|---|---|---|
| ☐ | D | 고정 구성 요소 열거가 inproc(session/engine 없음)을 구분하지 않음 | **스펙수정** — inproc/socket 기반 transport 구분 |
| ☐ | C | completion lane "terminal reply 전용" vs flow-state frame도 사용 | **스펙수정** — lane 용도 서술 확장 (06-auto-hwm 동일 서술도 함께) |
