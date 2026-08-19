# Relocation 캠페인 통합 체크리스트

작성 2026-08-19. 이 문서가 진행 중인 모든 작업의 단일 추적처다. 항목 완료 시 체크하고
커밋 해시를 병기한다. 새 사용자 지시는 이 문서에 먼저 반영한 뒤 착수한다.

## 진행 규칙

- **에이전트 운용(사용자 확정 2026-08-19, 갱신 2026-08-19)**: 사용량 배분을 위해
  **에이전트 작업은 가급적 codex로** — 작업 실행 **gpt-5.6-terra**, 리뷰·검증
  **gpt-5.6-sol**. sonnet 병용 가능(환경 반복 디버깅 등 적합 시). terra의 종전
  "sandbox 제약"(vstest TCP bind·cmake 불가)은 plugin 기본 sandbox가 원인 —
  **`codex exec -m gpt-5.6-terra -s danger-full-access --skip-git-repo-check`로
  기동하면 테스트 실행까지 제약 없음(2026-08-19 실증)**. 감독(코디네이터)은
  판정·커밋·조율 전담. **가능한 한 최대 병렬**, 공유 worktree 파일 소유권 분리.
- 그린 마일스톤마다 즉시 커밋·push. 커밋 스테이징은 **파일 명시 나열**(공유 worktree
  오염 방지 — 광역 `git add` 금지). **에이전트의 `git reset`/`git checkout` 등 트리
  전역 복원 명령 금지**(2026-08-19 사고: terra의 reset이 타 에이전트 미커밋 수정을
  증발시킴 — 자기 파일만 개별 revert, 그린 즉시 커밋 요청으로 위험 창 최소화).
- 커밋 묶음마다 Codex sol 리뷰. 발견 결함은 이 문서에 항목으로 추가 후 해소.
- 스펙 변경은 오류·개선만. 상호 운용(cpp·java·dotnet·node 동일 프로토콜)은 프로젝트
  근간 — 충돌하는 스펙 조항이 오류다.
- 완료 조건: 이 문서의 A·B·C 전 항목 체크 + 모든 테스트 그린 + zoneworld 제외
  6샘플×언어 성공 + sol 전 문서 spec-gap 리뷰 통과.

## A. base/delta capture 제거 (사용자 결정 2026-08-19)

- [x] 정식 문서 제거 (spec 15·01·06, 언어 interface, 내부 51·52, guide 재생성) — `8b12fbf62f`
- [x] node 구현 제거 — `b05791682b`
- [x] java 구현 제거 — `14de957df8`
- [x] cpp 구현 제거 — `b2b1713927`
- [x] dotnet 구현 제거 — `5d02ef66b1` (-1643줄)
- [x] **wire 원자 커밋** — `20b914fcec`: schema 필드 2개+stage 타입 제거, self-test
      239→234, golden 9/7→7/3 수기 재계산, 4언어 codec 정리, 4게이트 그린
- [x] draft 파일 재삭제 완료 (2026-08-19 — untracked 복원본 제거, git 이력으로만 접근)

## B. 원 캠페인(M9) 잔여 검증 게이트

- [ ] cpp e2e ST-C4 — 구현 `284d78ca74`(identity-conflict variant 3/3 그린).
      **sol 2차 [H]로 체크 철회(2026-08-19)**: 현 variant는 독립된 두 public Join의
      통상 reservation 경합이지 config-10이 계약한 "동일 relocation identity의
      checksum/길이 불일치" assembly 충돌이 아님 — 계약 fault point 경유 재작성
      필요(cpp 트랙 배정)
- [x] cpp e2e Track F — **완료**: 구현 `284d78ca74`, relay 응답 3중 결함 수정
      후 SF-F2·F3·F7(3회)·F11 전부 그린, 4종 기본 all 게이트 편입(스킵 목록
      제거). 잔여 관찰: RelocateReq 최초 전달이 20s 요청 기한까지 지연 후 재시도
      착지 의심(부하 시 2회, 유휴 그린) — 하단 관찰 항목. 이력(2026-08-19,
      e38c63fcf3 트리):
      **SF-F2 2회·SF-F3 그린, 기본 all 게이트 편입**(run_e2e 정직화 — sol ⑥ 해소,
      스킵 명시 출력). **SF-F7 3/3·SF-F11 잔여 실패 = 신규 결함**: relocation·
      증거 전부 그린 후 교차 노드 message-follow relay의 probe 응답이 caller
      노드로 미귀환(request_to_spot/await_completion, mesh_node_runtime:2841) —
      fence/bind 계열 무관 실증. 부수 관찰: SF-F2 웜업 중 간헐 ~19s RelocateReq
      지연 2회(재현 안 됨). relay 응답 유실 해소 후 F7/F11 마감
- [x] **cpp relocation 후 정지 — 수정 완료 `9c4f5ec83e`**: 29초 자체는
      스펙 명령의 30s Message Follow 창(비결함). 진짜 결함은 fence 없는 로컬
      요청자의 파킹 요청이 pending 미기록→재생 terminal pending-miss로 응답 무음
      폐기→요청자 영구 대기(gdb·계측 실증). 수정: 파킹 조건과 동일 조건으로
      pending 기록(빈 fence 허용)+fence-less terminal은 활성 follow target 노드
      검증으로 수용, 회귀 유닛 고정. SF-F2 단대단 통과, 게이트 43/43(기존
      app_host SIGSEGV도 소멸) [발견·해소 2026-08-19]
- [x] Bingo 재검증 (2026-08-19): node·kotlin 각 3회 클린 실행 6/6 첫 시도 그린,
      relocation 실행 확인(대체·route-ready·target_resume), 지문 재현 없음 —
      당시 미커밋 편집 트리가 원인으로 종결 (로그 scratchpad/sample-gates/*-recheck-*)
- [x] dotnet 샘플 6종 실행 — 1차 4/6 그린(DeliveryDispatch·GameQuest·
      ShoppingMall·SupportChat), Bingo·TicTacToe는 cutover 회귀 수정
      `f859696dc7` 후 각 2회 exit 0 — **6/6 확보**(최종 HEAD 재확인은 G-2)
- [x] **dotnet push 유실 회귀 — 수정 완료 `f859696dc7`**: 진짜 원인은 relay
      가로채기(적색 청어)가 아니라 `6fa7d6aab8`의 직접 join cutover가 remote-join
      import에 canonical-maintenance 전용 활성화 헬퍼를 호출 → 예외로 분리 태스크
      조용히 사망 → 세션 라우트 커밋(44)·Join terminal 미발생 → 이후 push 전면
      무실패 유실. cutover 완료가 remote-join replay를 직접 구동하도록 수정(store
      복구 경로 반사), 회귀 테스트 A/B 고정+ProtocolErrors 불증가 단언.
      **후속(D급 기록): ① cutover 사망이 debug 게이트 로그+카운터로만 관측 —
      명시 실패 강화 **해소 `f2dfa809e8`**(cutover 사망 always-on TraceError;
      SendIfBoundToAsync는 SkippedNotBound 신설로 정직화, 설계상 drop 유지) ② cpp/java 동일 실수 점검 **완료(2026-08-19):
      양쪽 구조적 면역**(cpp는 actor 경로에 maintenance import 개념 자체 부재+
      전 단계 인라인 complete(failure)로 loud, java는 admission 분기에서 kind
      판별+direct 전용 헬퍼). 단 java 신규 위험 발견: command 44 라우트 전환이
      fire-and-forget warn-and-swallow(ZLinkActorSpotAdmission:568/729/749) —
      join accept 후 교차 노드 push 무음 유실로 퇴화 가능 → **해소(java: cmd-44
      단계를 join 체인에 편입, 스펙 20 §5/52 §5·dotnet/cpp 동순서, 테스트 고정)**
- [x] java/kotlin 샘플 집계 게이트: SampleReleaseGateContractTest(22/22),
      CurrentManagerFakeBackendTest(1/1), FORBIDDEN_SAMPLE_PATTERN rg 스윕(508파일
      0건) — 3/3 그린, live redis, run_samples.sh 동일 호출 재현 [2026-08-19]
- [x] RelocationBehaviorConformanceTests(dotnet) — **해소 `86ed4a02f6`**: hang은
      "실패 join의 rollback replay가 mailbox turn 영구 파킹→Dispose 대기" 위장.
      실제 결함 3건 수정: ① 직접 join이 durable row를 Committed 이상 미전진(+
      pointer/CRC 불일치) → Activated 전진+steady 정규화를 Join terminal 이전에,
      ② READY Pending 중복을 fault 대신 멱등 attach, ③ steady 행 leave 수용.
      낡은 fixture 3건 스펙 재정렬(ZLDR decode·READY 보존·leave 이벤트 재바인딩).
      9/9 무행, 전체 게이트 1770/1773(인가 실패만). **잠복 위험 D급 기록**: join
      실패 회귀 시 captured-frame replay가 동일 파킹으로 suite hang 위장 가능
- [x] dotnet TicTacToe JoinGameNotify timeout 재현 조사 — **재분류(2026-08-19)**:
      부하 flake 아님, 위 `6fa7d6aab8` push-relay 회귀의 결정적 증상으로 흡수 종결
- [ ] harness 기본 `all` 스테이지 깨끗한 단독 재실행 (동시 에이전트 경합으로 1차
      판정불가; message-follow "Raw MeshNode requires the host Application Job Queue"
      사전 실패 주장 포함 확인)
- [x] cpp bind-session 재시도 소진 분류 교차 언어 parity — **조사 완료·판정
      (2026-08-19): 4언어 전부 발산.** cpp=deadline_exceeded(46ef4b0f03),
      java=마지막 raw 예외 누출(ZLinkActorRetryScheduler:164), dotnet=마지막
      ZLinkFrameworkException 재던짐(보통 Unavailable, ZLinkSessionActorCoordinator
      :349), node=fire-and-forget errorSink만·무유형(actor-packet-relay.ts:546).
      스펙 32 일반 원칙(기한 내 미완료=DeadlineExceeded)상 cpp가 규범 —
      **판정: 3언어를 소진 시 DeadlineExceeded 유형화로 정렬**(스펙 무변경).
      **node 완료 `6a5b42b098`**(+nack 오분류 정정 `2a65d3aeea`),
      **java 완료 `c835984c89`**, **dotnet 완료(4언어 정렬 종결)**: retry 소진 시
      DeadlineExceeded+InnerException, 비재시도 실패는 무변환 통과, 테스트 고정
- [x] **ST-C2 session bind 실패 — 수정 완료 `e38c63fcf3`**: 재시도 복원(기한
      바운드, dotnet parity) + H2 실증·수정 — read_route_owner_fence의 하드코딩
      5s margin이 설정 3s TTL 초과로 admission lifetime ≤0 → 영구 stale. 전
      콜사이트가 설정 margin 전달, ST-C2 3/3 통과, 게이트 43/43. 원 기록: `8bae89dc0f`가
      stream_host_service의 stale-route bind 재시도(무효화→route 변경 대기→재진입,
      5s 기한)를 삭제하며 기계를 죽은 코드로 방치(retryable_outcome 미사용,
      bind_attempt=retry 무호출) → ST-C2 유일의 교차 노드 bind가 생성 직후
      authority 수렴 경합의 일시적 stale(conflict+actorLocationStale)에 1차
      종결. dotnet/java는 동일 경합을 재시도로 통과(주석 명문). **수정 방향:
      죽은 재시도 기계 재배선(stale=무효화+≤1s 대기+retry, not_ready=단기 재시도,
      기존 5s 기한이 deadline_exceeded 소진 보장)** — cpp 29초 공백 에이전트
      종료 후 투입. H2(수신측 fence read nullopt·5s 하드코딩 margin)는 재시도
      복원 후 트레이스로 판별
- [x] cpp standalone actor 직접 relocation 복원 갭 — **판정 완료(2026-08-19): cpp
      실결함.** 4언어·스펙 모두 "타깃의 기존 Entry Spot으로 이동"이 계약인데 cpp만
      restore에 target_spot=nullopt 하드코딩(stateful_object_runtime.cpp:1584),
      드레인 경로(app.cpp:3575)가 실패, 기존 m6c:3010 테스트는 materialization
      미배선으로 거짓 그린. **수정 완료 — `aff9511de6`**(Entry Spot 로컬 해석,
      aggregate 경로 무변경, 무crash 명시 실패, disabled-fallback 증명, 41/41 ×2)
      [D에서 승격 2026-08-19]
- [ ] m6b M-c mismatched-identity rejection 테스트 (aggregate identity-fencing 조사
      포함) [D에서 승격 2026-08-19]
- [ ] **관찰(저순위): RelocateReq 최초 전달 ~20s 지연 의심** — location_committed
      →RelocateReq 수신 간격이 harness 기본 요청 기한(20s)과 정확 일치 2회 관측
      (부하 하), 첫 전달이 기한 만료까지 침묵 후 재시도 착지 가설. 유휴 재실행
      그린 — 재발 시 전달 경로 추적
- [x] **[C] SIGFPE 크래시 수정**: actor Join relocation 시 소스 노드 크래시 —
      moved-from unordered_map<string,optional<message_t>> emplace(bucket_count 0),
      spot_runtime.cpp:3596 완료 경로 × task.hpp:259 continuation 경합, 간헐(2/4).
      **수정 완료 — `aff9511de6`**(진짜 원인은 dangling this: 8bae89dc0f의 임시
      코루틴 래퍼가 suspension에서 소멸 — 프레임 소유로 수명 명시화, 회귀 테스트
      포함; 캠페인 유발 아님) [발견 2026-08-19]
- [x] cpp preset 빌드 디렉토리 재생성 완료 (2026-08-19, /mnt/d/tools/vcpkg 완주 —
      전 타깃 컴파일 그린). 참고: 원 SIGFPE는 정상 빌드에서 미재현이나 aff9511de6의
      수명 수정은 결정적 회귀 테스트가 증명하는 실버그로 유효 유지.
- [x] ST-B1 프록시 실패 — **수정 완료 `4109e908d4`**: 기존 잠복 결함(cold probe가
      fence 없이 구 소유 노드 도착 시 릴레이 미경유 → 오염된 로컬 테이블 조회 409;
      Aug-12/15 커밋 기원, Entry Spot 수정으로 노출). 릴레이 합성 + current_actor_ref
      라우트 기반 보고 + disabled-fallback 증명 테스트. ST-B1 3/3·ST-A1 그린
      [발견·해소 2026-08-19]
- [x] 명시적 실패 후 source actor 영구 hang — **수정 `b55ebf12d9`**: 탈출 불가
      reconcile phase 함정. PREPARE 실패는 drain-replay 종결, FINALIZE 모호 케이스만
      reconcile+기한+스윕(무한 대기 구조적 불가). 단위 테스트 고정 [해소 2026-08-19]
- [ ] **ST-A3 결정적 실패**(기존 — ST-B1 수정 전후 동일 재현): 별도 timing/gate
      이슈, 원인 조사 필요 [발견 2026-08-19]
- [ ] **ST-B1 후속: 소스 Entry Spot on_leave_actor 미발화** — 동일 HEAD·동일 머신
      에서 에이전트 A는 11/11 마커 존재, 에이전트 B는 청정 독점 재빌드로 3/3 100%
      부재(sha256 d3f1f8b3ab…, 로그 20260819-062437-2768634). 환경 가설 기각 —
      **순서 경합 의심**(source_cleanup이 leave 명령 dispatch보다 먼저 정리하는
      fire-and-forget 창; 스케줄링 프로파일에 따라 항상 이기거나 항상 짐).
      **수정 완료 `4437f886a8`**: OnLeave를 node 레벨 알림으로(3번째 entry-spot
      주소성 갭 해소, exactly-once 테스트) + [H] cold-probe relay 3/4 identity 보존
      (deadline은 30s 관례 — 절대기한 전달 메커니즘 부재, D급 후속 기록).
      [C] reconcile 3분기(store 조회 기반) 랜딩 `653af3f8ab`.
      **재검증(2026-08-19, 청정 재빌드 sha256 9066811…)**: leave 마커 3/3 존재 —
      leave 수정 유효 확인. 잔여 실패는 시나리오가 base `8bae89dc0f`에서 제거된
      `commit_request`/`commit_ack` wire packet 어휘를 단언하는 harness 노후화
      (commit=mesh packet→store CAS 재구조화; 스펙·타언어에 해당 어휘 전무 확인).
      **판정: ST-B1/B2/B3/C2 시나리오+feature-map을 `location_committed` 기반
      증거로 갱신(의도 보존: commit-before-joined 순서·correlation) — e2e
      에이전트 진행 중** [발견·해소 2026-08-19]
- [ ] **sol 전 문서 spec-gap 리뷰**: 스펙·guide·e2e·언어 interface 전체 vs 구현 대조,
      gap 0 확인 (완료 조건)
- [ ] **sol 문서 예비 리뷰(2026-08-19, 기준 bec7a9e48a) — 11건 발견·전량 배정**:
      ① [M] cmd-44 "commit" 문구 모순 → **해소(`9077314a7e`)** ② **[C] node authority allocation 레코드
      비규범**(target/spotKind/capacityBundle vs 규범 descriptor/descriptor
      LifecycleGeneration/capacity; 골든 테스트가 encodeAuthorityRecord 우회로
      거짓 그린) → **해소 `52caf2aaca`**(규범 형태 수렴+골든이 실제 writer 구동) ③ [H] cpp 28 발신 게이트 상시 닫힘(생산
      경로 JSON 고정) — C-7 라이브 검증 결과와 함께 wire join 생산 선택 여부
      최종 판정 ④ [H] cpp cmd-44 재전송이 스펙 18 §487(무재시도 one-way) 위반
      — cpp interface 문서(06:191)가 구계약(고정 간격 재전송) 잔존, 공통 스펙
      우선: cpp 재전송 제거+문서 정정(C-7 후) ⑤ [H] node managed-stream bind
      기한 소진 오분류 → **해소 `52caf2aaca`**(DeadlineExceeded+cause 보존)
      ⑥ [H] node 공개 Session-Actor 인터페이스 불일치 → **해소 `52caf2aaca`**
      (bind=ActorRef 전용, relay(dispatch,…) overload 신설) ⑦ [M] 스펙 21 상태 주석 → **해소(`9077314a7e`)** ⑧ [M] bind reference
      DeadlineExceeded 누락 → **해소(4언어 en/ko, 문서 커밋)** ⑨ [H] config-10 Track E1A~C/G/H/I 미구현·⑪ [H] config-6
      26중 14만 지원 — **F(e2e_inventory backlog, 사용자 확정 별도 세션) 범위로
      분류** ⑩ [H] ST-C4 checksum variant — 기존 B 항목에서 추적 중
- [ ] 최종 게이트 일괄: 4언어 unittest + 6샘플×언어 + doc 게이트 + 최종 보고

## C. 상호 운용 확장 (M10, 사용자 확정 2026-08-19 — 근간·양보 불가)

- [x] C-1 store 규범 계약 설계 비준 (2026-08-19): generic opaque = dotnet/java 공유
      ZSET+cmsgpack log 채택(값은 raw bytes — java의 내부 base64 제거), 키 =
      `{prefix}:{zlink-location-v3}:opaque:{sha256hex(논리키)}`; descriptor·lease·
      client-server·fanout·authority = "canonical JSON over opaque record" 단일 규칙
      (논리키 preimage 규범화, java 전용 Lua 경로는 붕괴·인덱스는 private 보조);
      relocation blob = raw STRING PSETEX, `{zlink-relocation-v1}` 독립 버전 태그;
      마이그레이션 = **clean break**(형식 판별자 0x01+recordVersion, 미인식 버전
      명시 실패; 기존 Redis 상태 drain 필요 — 최종 보고 명기). 검증 조건 2건:
      java Lettuce 8-bit ARGV 실증(Phase B), authority 행 opaque 경로 byte 검증(C-2 전).
- [x] C-2 스펙 개정 — `a3e6144ea3`: 21 §1.2/§2.4 신설, 실제 경계 문구가 있던
      22 §7을 MUST 수준으로 재작성, 23 §8 relocation blob 규범화 (28은 무수정 유지)
- [x] C-2b authority 판정 (2026-08-19): node도 단일 행(4언어 중 3)이며 진짜 발산은
      objectGeneration 의미(전역 시퀀스 3/4 vs dotnet identity별 영속). **판정:
      단일 행 + 전역 시퀀스 표준화** — 전역 단조 카운터가 identity별 단조성을 자동
      보장하므로 dotnet GenerationKey는 중복, meta/payload 분할의 read-verify-retry
      도 제거됨. dotnet 수렴 시 검증 필수: "현재+1" 산술 가정 CAS 전수 확인,
      PayloadSha256(분할 읽기 보정용)도 함께 제거. §2.4 authority 절 확장은 C-3에
      포함(golden에 authority 스키마 필요).
- [x] **C-3b descriptor payload 전-필드 규범 스키마 고정(2026-08-19 신설, 3언어
      차단 해소)**: golden의 descriptor는 예시 최소형 — node/dotnet/java가 각자 형태
      발명 중이던 것을 java 정찰이 적발. 스펙 21 §2.3 논리 계약+4언어 실제 타입에서
      규범 필드 도출→§2.4 JSON 스키마 명문화→golden 전-필드 벡터→4언어 골든 갱신.
      **완료 `74a0ed04da`**(§2.4 전-필드 표+golden 6키/7값, 중복 generation 규범
      제외 판정, 4언어 골든 무변경 그린)
- [x] C-3 store 레코드 golden fixture — `bdc3e8a8c5`: 6키/5값 벡터(redis Lua cmsgpack 실검증), 21 §2.4 authority 스키마·22 §7 확장, 4언어 소비 테스트 즉시 그린. C-4 명시 이월: cpp authority payload hex→base64, encode측 스탠딩 테스트
- [x] C-4 4언어 store 구현 수렴 — **완료(2026-08-19)**: java C-4a·node C-4b·
      dotnet C-4c/e·cpp C-4d 전 슬라이스 마감, 4언어 실제-writer 골든 conformance
      그린. 진행 기록: **키 브레이스 판정(2026-08-19): Cluster
      hashtag 리터럴 유지** — golden이 오독으로 brace-less 고정했던 것 정정
      `8a3804a110`. **java 슬라이스 1차 `326833810b`**(base64 제거=Lettuce codec
      문제였음·0x01 태그·PSETEX blob·프로덕션 encode conformance, 모듈 그린).
      **node 1차 `eb3d74f6cc`**(키·opaque 로그·blob; envelope·시퀀스는 C-4b),
      **cpp 완료 `e14bce0297`**(전면 재작성+hex→base64+encode 검증, location 7/7,
      live redis 실키 확인). dotnet 슬라이스 게이트 대기 중.
- [x] C-4b node 마감 — **완료 `165b219638`**: canonical envelope 5종+전역
      objectGeneration+strict recordVersion. 수렴 회귀 원인은 aggregateGeneration
      string/bigint 디코드 타입 버그 — 수정 후 re-read-adopt 수렴, 2-repo in-memory
      테스트 고정, **코디네이터 redis 검증 56/56 그린**. 소형 이월: aggregate owner
      -generation의 전역 시퀀스화는 marker CAS와 원자적인 provider Lua op 필요(D급).
      원 항목: ① canonical JSON envelope 전환(값이 아직 구 필드 형태 —
      키는 맞으나 타 언어가 파싱 불가, location-store-repository.ts 3591줄 CAS 재작성),
      ② objectGeneration을 identity별 카운터→store 전역 시퀀스로(reserve() ~:965 —
      기존 감사의 "node는 이미 전역" 기록은 오류였음) [node 1차 eb3d74f6cc에서 이월]
- [x] C-4c dotnet Lua byte 정합 — **확정·수정 `d55d568e6b`**: 무태그 cmsgpack,
      -1 센티널, 정수 tombstone, 빈 tombstone version 전부 실재 → 수정·라이브 byte
      검증 [node 예측 적중]
- [x] C-4e dotnet store 마감 유닛 — **완료**: ① authority 키 canonical preimage
      `0c418de4b5`(fixture 4개소 이관, fault-injection double 2건), ② descriptor
      전-필드 표 정렬 `a7309bb96d`(C-4f: DescriptorRecord recordVersion 도입·중복
      generation 제거를 provider VersionOf로, 실제-writer 골든 3종)
- [x] C-4d cpp store 마감 — **완료 `bc3b27a750`**: storeVersion을 provider 버전
      유도로 재설계(예측 카운터 6콜사이트 삭제, 소비자 무변경 검증), envelope 정렬,
      routing id hex 오변환 실버그 수정, 실제-writer 골든(mesh·authority-actor),
      게이트 42/43 ×2(app_host 기존)·redis 라이브 그린. 원 항목:(2026-08-19 신설 — 앞선 'cpp 완료' 판정 정정):
      cpp의 mesh/authority 실제 writer가 golden 비정합(여분 storeVersion이
      public_host_runtime의 wire-echo 펜스로 load-bearing, target/bundle/mesh
      필드명 다수 발산). 수정 유닛: envelope 필드 정렬 + storeVersion 펜싱을
      reservation_id 기반으로 재설계(public_host_runtime 소비자 동반 수정) +
      mesh/authority 실제-writer 골든 conformance. F2(Lua 태그) F3(lease writer)은
      b20d2011fe로 선랜딩. (JSON 키 순서는 규약상 field-compare라 비결함)
- [x] C-4a java 수렴 — **재정찰로 지형 재정의(2026-08-19)**: terra 감사의 Lua-HASH
      스택은 실은 **죽은 코드**(프로덕션 미도달, 자기참조+테스트 1개뿐). 라이브
      경로는 core의 ZLinkProviderDescriptorRepository/ZLinkProviderLocationRepository
      — 자체 비규범 스킴(zlink:v11: 프리픽스, PascalCase JSON, 이진 generation 접두,
      recordVersion 부재). **재범위**: ① provider 저장소를 §2.4로 포팅(node 참조
      구현, 타입별 단계·게이트·커밋), ② 죽은 Lua 스택은 oracle 커버리지 확인 후
      일괄 삭제(POSDDD), ③ 골든은 실제 writer 구동. **완료(2026-08-19)**: oracle
      82731d819d → mesh 708e2c3a79 → lease 0111e5e041(공유 codec) → client-server
      8272443a47 → fanout+레거시 기계 삭제 0f04ee8db7 → authority 값 cd40ab29f3 →
      authority 키+감사+conformance 441b60574c. **java 5타입 전부 canonical,
      골든 conformance 6종 실제-writer 구동, 모듈 그린.** 라이브 경로 동작 사실
      3건은 스펙 판정 후보로 유지(동일 revision RENEW=IGNORED_STALE, authority
      commit이 대상 lifecycleGeneration 미펜스, removeAllByOwner descriptor 미회수).
      fixture 소정정 1건: authority-spot 벡터의 active+pendingCreation 조합은 실제
      commit 불가 — reserved로 정정 필요(descriptor:mesh:*, owner-lease:* 등 →
      canonical-JSON-over-opaque) + ZLinkRedisAuthorityClient(3029줄) counter/CAS
      감사·수렴 — **상호 운용에 필수**(전용 경로가 남는 한 java descriptor를 타
      언어가 못 읽음), 전담 세션 규모로 분리 [java 1차에서 이월]
- [ ] C-5 cpp actorJoin 연산 — 요청 codec `134f22282c` 랜딩. **계약 판정(2026-08-19):
      schema 무변경(옵션 b)** — actorJoin(28) body가 곧 계약(node 원생 상호운용),
      transferId는 JSON 프로토콜 사설 부기, prewarm은 actor-정체성 키잉(node
      레지스트리 방식). 수신측=erased join 경로+정체성 키 파킹/newest-wins, 발신측
      =fence 게이트(기본 JSON 유지). **increment 1 `bffffc5377` + increment 2
      랜딩(수신 drain·fence 게이트 — 게이트 기본 닫힘 grep+테스트 증명). 이월:
      wire-admitted actor의 route 바인딩, chunk limit의 relocation 소비자 연결,
      전체 host-fixture e2e — C-7 라이브 검증에서 함께 판정** (service-wire cross-node join 요청/응답,
      binary tail 탑재 — codec은 `7ed3992ccd`로 기구현, node 발신 경로 참조)
- [x] C-6 dotnet actorJoin 발신 — **완료(terra 구현 + sonnet 검증·수정)**: 디코더
      오류 분류 버그 수정, cpp와 byte 레이아웃 완전 일치 검증, 게이트 닫힘 증명
      (generation-0 계약 의존 — 전용 게이트 테스트는 D급 후속), 1756/1759 그린
- [ ] C-7 harness 교차 언어 relocation stage 전 쌍 그린 — **1차 라이브 검증
      (2026-08-19, HEAD bec7a9e48a) 결과: 기본 게이트 8/19 적색 + relocation 3쌍
      전부 store 계층 실패. 하니스 드리프트 수정 `dec1c2dd9a`(message-follow
      구주장은 드리프트로 판명·그린). 결함 8군 전량 배정:**
      ① **[C] ClientServer admission 프레이밍 분열**: cpp는 hello/admit을 plain
      frame으로, node/dotnet은 RequestSeq 프레임 요구(무seq hello 무시/폐기) —
      cpp↔node/dotnet channel 3스테이지 전멸. 규범 판정+정렬 필요(스펙 대조)
      ② **[H] dotnet channel direct 경로가 admission-ready 대기 없이 즉시
      NotFound**(dotnet↔dotnet도 실패, RequestToChannelDirectAsync:7945)
      ③ **[H] node channel reply 상관 회귀**(node↔node: 서버 처리 완료, 클라이언트
      promise 미해결·루프 드레인 exit 0) ④ **[H] cpp fanout publisher 강제
      discovery 경로**(enable_publisher가 discovery=true 고정, channel_runtime
      :982)+cpp→X fanout 미전달 잔존(X→cpp는 정상) ⑤ **[H] cpp↔dotnet spot-route
      양방향 실패**(타 조합 전부 그린 — dotnet admission 의심, 42ddf3d2f4/
      8bae89dc0f 후보) ⑥ **[C] node mesh-node descriptor 스키마 미수렴**(rid/
      populationCapacity/updatedAt ISO vs 규범 routingIdHex/capacity/
      updatedAtEpochMs/lifecycleGeneration — C-3b 74a0ed04da 위반; node→dotnet
      decode 크래시) ⑦ **[H] java u64 signed parse**(Long.parseLong에 전범위
      u64 → java↔dotnet ~50% 실패; ZLinkProviderDescriptorRepository:648,
      ZLinkRedisRelocationStore:224, ZLinkRedisOpaqueLocationStore:855)
      ⑧ **[H] dotnet이 java 기록 lease/counter 값 파싱 실패**(개행/빈 문자열
      FormatException — 기록측 vs 파싱측 판정 필요). 원 항목: (JoinEntrySpot
      경로 우선, 기존 opt-in 스테이지 2907df293f/c43758fc05 기반)
- [ ] C-8 교차 언어 스테이지를 harness 기본 `all` 게이트에 편입 (회귀 구조 차단)
- [ ] **C-9b sol 2차 배치 리뷰(2026-08-19) — 발견 9건 전부 배정, 해소로 마감**:
      ① [H] cpp 28 수신 승인 전용 위반 → **해소 `938f68a658`**: admit_wire_actor_
      join이 승인 전용 스테이지만 수행(설치/CAS/membership/commit은 기존
      coordinator 단계에), later-attempt-wins 구현(로컬 유도 transfer id),
      게이트 계속 닫힘 ② [H] dotnet 28 발신이 승인 응답을 완료된 Join으로 처리
      — 직접 join durable phase 전진(`86ed4a02f6`)으로 완료 시퀀스가 스펙 정렬,
      C-7 라이브 검증에서 최종 판정 ③ [H] cpp actorJoin 실패 분류 붕괴 →
      **해소 `938f68a658`**: typed outcome(protocol_error/deadline_exceeded/
      unavailable)+application reply·chunk limit 표면화(소비자 연결은 이월 유지) ④ [H] java가 relocationFailed
      (53) 코드 무시하고 generic IllegalStateException(ZLinkCanonicalRelocation
      StateMachine:685) — 4언어 수신 분류 parity 파괴 → **해소: emit 표의 역표를
      상태기계에 공치(共置)해 typed kind 수신(원격요청 표 재사용은 parity 파괴라
      기각), 게이트 그린**
      ⑤ [H] ST-C4 계약 variant 미검증(B 섹션에 반영) ⑥ [H] run_e2e.sh가 Track F
      미실행인데 집계 성공 보고(:68-73) — cpp 트랙 ⑦ [M] dotnet authority/owner-
      lease 리더 recordVersion 미검증(spec 21 §420-425 fail-closed), cpp는 부재
      버전 허용 — 각 언어 마감 → **dotnet 해소 `f2dfa809e8`**(owner-lease 8개소+
      authority 깔때기 fail-closed, 테스트 2종) + **cpp 해소 `938f68a658`**(부재
      recordVersion 거부, parse_canonical_record 단일 헬퍼, 전 canonical 리더) ⑧ [M] node acknowledged 경로가 즉시 거부를
      DeadlineExceeded로 오분류+production이 예외 폐기(session-actor-coordinator
      :168) → **해소: nack은 응답 errorKind 디코드(RequestFailed 폴백),
      DeadlineExceeded는 transport 기한 경로 전용; swallow는 스펙 20 근거로
      errorSink 관찰화(바인딩 유지·롤백 금지), 137/137 그린** ⑨ [L] feature-map ST-B2 행 "commit ack" 잔재 —
      코디네이터 즉시 수정
- [ ] C-9 상호 운용 신규 코드 sol 리뷰 + POSDDD 패스 — **1차 sol 배치 리뷰 완료
      (2026-08-19), 발견 5건 전부 배정**: [C] reconcile 기한 스윕의 relay-ready
      비가역성 위반(→판정 정정: 기한 도달 시 Location Store authority 조회로 확정
      타깃 추종/소스 복원/명시 unavailable 3분기 — 전문 에이전트), [H] authority
      실제 producer가 4언어 모두 golden 비호환(cpp 여분 storeVersion·node 구
      envelope·java 구 hash·dotnet 3키 — 각 슬라이스에 배정), [H] cold-probe 합성
      follow가 op identity/deadline/source/reply-route 4값 미보존(전문 에이전트),
      [M] cpp Lua point-read 0x01 미검증(cpp store 에이전트), [M] golden 테스트가
      실제 producer 미구동+dotnet brace-less 잔재(각 언어 마감에 편입)
- [x] C-10 node relocationFailed 세분화 — **완료**: java 기준표(97fc074058)와
      전 행 정합(22행 conformance 테스트 고정), 82/82 그린. 초안의 (15/33/34)는
      실제 기준 매핑으로 대체(34는 어느 언어도 인코딩 안 함; InvalidOp→21/33).
      **wire 어휘 확장 재판정: 불요** — 4언어 closest-fit 합의로 상호운용 모호성
      없음, 어떤 디코더도 해당 뉘앙스에 분기 없음, 스펙 15 실패표에 해당 행 부재
      → 스펙 오류 아님, 확장은 무행동 churn [2026-08-19]

## C-11 스펙 이슈 전량 확정 (사용자 지시 2026-08-19 — 보류 금지, 즉시 판정·반영)

- [x] 8건 판정·반영 완료 — 커밋 참조: ① 불확정 FINALIZE 3분기 복구 절차(28
      명문화), ② relay hop deadline=클라이언트 관리+로컬 창 재바운드(28 개정),
      ③ Entry Spot 비주소성+노드 레벨 알림(15/28), ④ 동일 revision RENEW=무해
      no-op(21), ⑤ authority commit은 자기 정체성만 펜스(21), ⑥ removeAllByOwner는
      authority만(21), ⑦ actorJoin(28) body=완전 계약(51), ⑧ golden authority-spot
      벡터 state→reserved 정정. (기존 목록 잔여: 28↔21/23 모순=개정 해소, TTL·
      pre-Prepare=base/delta 제거로 소멸, wire 어휘=현행 유지 확정, actorJoin
      originate=구현 중)

## E. 확정 후속 단계 (사용자 승격 2026-08-19 — 완료 조건 포함, C 완료 후 착수)

- [x] E-1 dotnet 성능 패스 — **완료 `42ddf3d2f4`**: 6건 전부(중복 lock 10개소 해체, byte-wise
      RoutingId comparer 16개소, HeldRecords O(N²)→상각 O(1), participant 단일
      dictionary, codec 삼중 복사→단일 span 복사, spot-participant 쿼리 캐시).
      4·6번 위치는 에이전트 식별(원 커밋 미기록) — 판단 근거 문서화. 전 게이트
      1770/1773+conformance 9/9, 무동작 변경
- [ ] E-2 cpp complete_relocation_assembly 구조 분해 — terra 시도는 정직 실패
      (sandbox cmake configure 불가·등가성 테스트 기준 미충족, 미완성 변경 revert
      완료). **로컬 sonnet으로 재투입 예정**(direct-call harness 우선 구현)
- [x] E-3 dotnet participant-restore 추출 — **완료 `42ddf3d2f4`**(순수 이동, staging은
      오케스트레이션 가독, rollback 부기 불변)
- [ ] E-4 cpp aggregate 오케스트레이션 reason 전파 (task_t<bool> → 분류 전달)
- [ ] E-5 E 단계 전체 sol 리뷰 + 전 게이트 재확인

## F. 별도 캠페인 (이 캠페인 범위 외 — 착수 시 별도 계획 문서)

- e2e_inventory 기존 backlog 168건: relocation과 무관한 14개 문서 전반의
  교차참조·feature-map 부채. **사용자 확정(2026-08-19): 본 캠페인 전체 완료 후
  별도 세션에서 진행.** 이로써 미결 사용자 결정 0건 — 잔여는 전부 실행.

## G. 최종 완료 게이트 (사용자 지시 2026-08-19 — 모든 섹션 완료 후 마지막에 일괄 실행)

- [ ] G-1 전체 unittest 4언어 일괄 그린: cpp ctest(framework-unit|contract 전체,
      알려진 환경 제외만 허용·사유 명기), dotnet 전체(conformance 처분 결과 반영),
      java gradle 전체, node npm 전체 — 각 언어 최종 HEAD에서 연속 실행, 결과 로그 보존
- [ ] G-2 전체 샘플 실행 성공(zoneworld 제외): 6샘플(Bingo, DeliveryDispatch,
      GameQuest, ShoppingMall, SupportChat, TicTacToe) × 4언어+kotlin 전부
      최종 HEAD에서 재실행, 종료 코드 0 확인·로그 보존 (중간 검증과 별개로
      마지막에 반드시 1회 전체 재실행)
- [ ] G-3 java/kotlin 샘플 집계 게이트·doc 게이트·harness all 스테이지 최종 확인
- [ ] G-4 G-1~G-3 결과를 최종 보고에 매트릭스로 첨부
