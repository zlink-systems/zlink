# 0.17.0 DONTWAIT 캠페인 — 세션 핸드오프 (2026-09-04)

> **왜 이 문서가 있나:** WSL을 C→D 드라이브로 옮기며 워크스페이스 `/home/hep7/project/zlink`를
> **삭제 후 재-clone**한다. 이 세션의 진행상황을 새 세션이 이어받도록 정리한다.
> **origin에 푸시된 것만 살아남는다** — 로컬 미푸시/미커밋은 전부 소실된다.

진실원천: 이 문서 + [`dontwait-0.17.0-framework-completion.ko.md`](../../dontwait-0.17.0-framework-completion.ko.md)
+ 게이트 정의 `doc/plan/core-send-dontwait-completion-0.16.0-plan.ko.md` line 143
(= framework unit + cross-language E2E + 공통 sample 7개 모두 통과).

기준선(전부 green): `seven-samples-green-v1` (4bad5ac979, 2026-08-29).

---

## 1. 완료·푸시됨 (origin/main = `8f8f75ff71`)

| 커밋 | 내용 |
|---|---|
| `4d263e66b9` | **DONTWAIT framework fix** — DONTWAIT 전송을 바인딩 async send 경로로 보내고(cpp `.async()`, dotnet `TrySubmit()`, node `submitBindingAsyncSend`, java `submit()`) 공개 poller가 `POLLOUT\|POLLCOMPLETION` 구독해 WRITABLE completion을 drain. 4언어. dotnet peer-epoch fallback, java state-lane 이동 포함. |
| `3f0f02d478` | **docs PR #2 머지** — 영문 가이드/스펙 자연화(67 `.en.md`) + 폐기 `archive/` 삭제(47+47) + README.ko archive 참조 정리. main과 완전 분리(충돌 0)라 B의 0.17 바인딩·plan 문서 전부 보존됨. |
| `baa4b4e4f6` | **언어별 가이드 재생성** — PR이 공통 소스만 자연화하고 `generate_language_guides.py`를 안 돌려 생성물 드리프트 → docs 배포가 "Check generated language guides" 실패. 재생성(2패스 수렴)해 해소. |
| `8f8f75ff71` | **앵커 수정** — 자연화가 04 §8 헤딩을 "…— Joining Two Stages Without a Gap"로 재작성해 슬러그가 바뀌었는데 07-serial-executor-layers:256 상호참조가 옛 앵커를 가리킴. check_doc_links 1건 → 새 슬러그로 수정. |

**✅ docs 사이트 퍼블리시 완료** — GitHub Pages 배포 성공(docs.yml run `33872868376`, Deploy to GitHub Pages => success). zlink.systems 서빙.

---

## 2. 해소된 조사 (오해 정정)

- **"cpp async-only submit projection is missing" 계약 체크 실패 = 로컬 false-failure(환경 아티팩트).**
  동일 스크립트가 마지막 green 배포 커밋(0377213d26)에서도 로컬 실패하지만 **CI에선 통과**한다
  (해당 배포의 "Check framework document contracts" 스텝 = success, 그리고 앵커 수정 후 배포도
  전체 성공). cpp framework엔 이미 public `task_t<void> async()` 종결자가 있다
  (`framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:455`,
  계약 문서 `.../languages/cpp/interfaces/*.ko.md`에 9곳). 체크 regex만 `submit`을 요구해 어긋나 보였을 뿐.
  → **framework API 추가 불필요.** (세션 중 "framework에 submit 종결자가 없다"고 한 초기 보고는 정정됨.)

---

## 3. 게이트 재검증 결과 (4d263e66b9 + B 최신 바인딩 review-fix 대비)

4언어 병렬 codex(sol/terra)로 재검증. node·java는 완료, **cpp·dotnet은 세션 중단으로 미완**.
상세 산출물은 이 디렉토리에 보존: `fw-gate-node-summary.md`, `fw-gate-java-summary.md`,
`cpp-job-tail.txt`, `dotnet-job-tail.txt`, `inflight-bucketB-probes.patch`, `fw-gate-reverify.prompt.md`.

| 언어 | DONTWAIT | cross-lang E2E | bucket-B | 잔여(pre-existing 등) |
|---|---|---|---|---|
| **node** | ✅ 해소 (backend-contract 56/56) | 미확정(runner가 요약 전 temp 정리) | 없음 | ESLint `spot-timer.ts:137`이 npm test 차단; 샘플 SupportChat 실패(브라우저 하네스 거부-관찰 이슈, framework/Core 아님) |
| **java** | ✅ 해소 | ✅ **4/4** (Java↔Node/.NET 양방향) | 없음 | 2×M6A + JavaDocumentationRegression(전부 pre-existing); 샘플은 play-a `TEARDOWN_FAILED`로 중단 |
| **cpp** | (부분) | — | **미해소** | **m6b 여전히 실패**: `transport_failure.error_kind()==deadline_exceeded` assertion. 잡이 넣은 임시 probe(`not_found→route_unavailable`)로 **미해결**(inflight-bucketB-probes.patch) |
| **dotnet** | (부분) | — | **미해소** | stale-authority 조사 중 디버그 로깅만 추가(수정 아님, dotnet-job-tail.txt). 샘플 ShoppingMall 실패 로그 있음 |

> **inflight-bucketB-probes.patch의 2편집(cpp raw_route_port.cpp, dotnet ManagedMeshNode.cs)은
> 미검증이라 main에 커밋하지 않았다.** cpp편집은 m6b를 못 고쳤고, dotnet편집은 로깅 프로브일 뿐이다.
> 다음 세션은 참고만 하고 근본원인부터 다시 판정할 것.

---

## 4. 남은 작업 (우선순위)

### 4.1 [OPEN] bucket-B terminal/error 분류 클러스터 — 캠페인 핵심 잔여
- **cpp m6a** `records.size()==1` (bound-session bind), **cpp m6b** `error_kind()==deadline_exceeded`.
- **dotnet stale-authority** `TimedOut(101)` vs stale-terminal `(107)`.
- B의 바인딩 review-fix로 **해소되지 않았다.** 근본원인(framework 매핑 오류 vs Core vs 바인딩) **미판정.**
- **정책:** Core 버그면 A(나)가 직접 수정+커밋+푸시(sub-agent 활용). Core는 B가 perf만. 바인딩은 B 소유.
- 착수법: `zlink-work/c016/fw-gate-reverify.prompt.md`(이 디렉토리에 사본) 패턴으로 cpp/dotnet 재조사.
  실제 반환 코드/errno vs 기대값을 매핑 지점(`file:line`)까지 추적. m6b는 bound-session bind가
  retryable outcome을 `deadline_exceeded`로 분류해야 하는데 다른 terminal로 분류되는 지점을 찾을 것.

### 4.2 [OPEN] framework 가이드에 "메시징 API 종결자 의미·사용법" 추가 (사용자 요청, 미착수)
- **현황 진단:** 의미는 **부분적으로만** 설명됨 —
  - `03-concepts.ko.md` §7 (line ~274): "완료의 뜻은 두 갈래" 한 문단 요약.
  - `04-backpressure.ko.md` §3.1 "send가 `async`인 이유"(line 122~275): **가장 깊은 실질 설명**이나
    'backpressure' 맥락에 묶여 있음. §3.2가 request timeout 경계.
  - `05-channel-messaging.ko.md`: 모든 예제에서 종결자(`.async()`/`.submit()`/`.await()`/`.Async<T>()`)를
    **쓰지만** 종결자라는 **개념 자체를 설명하는 절이 없다.**
- **제안 위치:** `05-channel-messaging`(메시징 API 가이드) 상단에 짧은 종결자 개념 절 신설 —
  (a) 모든 메시징 빌더는 종결자 호출로 끝난다, (b) 두 종류: one-way/publish 종결자(수락 시 반환값 없이
  완료) vs request 종결자(reply | timeout | route 오류), (c) 언어별 종결자 이름은 language-tab 코드블록으로,
  (d) 깊은 "왜 async/backpressure"는 04 §3.1로, 정확한 이름은 인터페이스 카탈로그로 크로스링크.
  04 §3.1과 **중복 금지**.
- **주의(정본 문서 규율):** common prose는 언어별 이름 금지(`check_prose_neutrality.py`) — 종결자 이름은
  코드탭에만. `.ko.md`+`.en.md` 양쪽 + `generate_language_guides.py` 재생성 + docs 게이트 통과 필요.

### 4.3 [OPEN] 샘플 게이트 실패 판정 (pre-existing vs 회귀)
- node SupportChat, java 샘플(play-a TEARDOWN_FAILED), cpp ShoppingMall — `seven-samples-green-v1`(4bad5ac979)
  대비 pre-existing인지 이번 캠페인 회귀인지 실측 확인.

### 4.4 [최종 게이트]
- 4언어 framework unit(DONTWAIT 회귀 0, 잔여는 pre-existing만) + 7 samples × 4언어 + cross-language E2E 전부 green.

---

## 5. 새 세션 빌드/실행 환경 (재-clone 후)

- **core/build-dev 소실됨** → 재빌드: `scripts/build-core.sh dev` (dev/no-LTO, 0.17.0). Core는 자체 빌드 금지 대상 아님(A가 필요시 빌드). **캠페인 중 Core 소스 수정은 A만(버그 수정), 성능은 B.**
- `/tmp` 기본 8G tmpfs — LTO/빌드가 고갈시킴. `TMPDIR=/dev/shm` 사용 또는 `sudo mount -o remount,size=32G /tmp`(무암호 sudo 가능).
- 게이트 락: `/tmp/zlink-{dotnet,jvm,node}-gate.lock` (`flock -w7200`).
- **언어별 동시 실행 안전**(독립 redis·ephemeral 포트) — 순차/헤징 금지. 이슈는 병렬 codex로 동시 수정.
- **codex:** `gpt-5.6-sol`(적절한 추론레벨) + `gpt-5.6-terra`. **ultra·fast 금지.**
- remote는 커스텀 SSH host `github.com-zlink` → `gh`는 `-R zlink-systems/zlink` 명시 필요.
- 커밋 스코프 가드: `core/**`·`bindings/**`·`core/doc/spec/**`·`framework/doc/framework/**`는 A가 직접 판단해 수정(에이전트는 금지). `git add`는 파일 명시(never -A). doc/site 절대 금지.
- 정본 doc 트리(`framework/doc/framework`, `core/doc/spec`)는 chmod 잠금될 수 있음 — 수정 시 `chmod -R u+w` 해제, 종료 시 재잠금(spec-server-reorg 규약).

## 6. 로컬 워킹트리 주의 (이 워크스페이스 한정, 곧 삭제됨)
- 이 세션 마지막에 로컬 main을 origin/main으로 ff하려다 정본 문서 chmod 잠금 때문에 부분 실패로 엉킴.
  **삭제 예정이라 방치.** 재-clone하면 깨끗한 8f8f75ff71을 받는다.
