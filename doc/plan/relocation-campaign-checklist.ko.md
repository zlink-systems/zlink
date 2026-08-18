# Relocation 캠페인 통합 체크리스트

작성 2026-08-19. 이 문서가 진행 중인 모든 작업의 단일 추적처다. 항목 완료 시 체크하고
커밋 해시를 병기한다. 새 사용자 지시는 이 문서에 먼저 반영한 뒤 착수한다.

## 진행 규칙

- **에이전트 운용(사용자 확정 2026-08-19)**: 작업 실행은 **claude sonnet + codex
  gpt-5.6-terra**(무거운 구현·심층 디버깅은 terra 우선), 리뷰·검증은 **codex
  gpt-5.6-sol**. 감독(코디네이터)은 판정·커밋·조율 전담. **가능한 한 최대 병렬**로
  진행하되 공유 worktree 파일 소유권을 분리한다.
- 그린 마일스톤마다 즉시 커밋·push. 커밋 스테이징은 **파일 명시 나열**(공유 worktree
  오염 방지 — 광역 `git add` 금지).
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

- [ ] cpp e2e ST-C4: 작성된 identity-conflict variant를 안정화된 트리에서 실행·그린
      확인 후 커밋 (checksum variant는 test seam 부재로 정직한 부분 — C-4와 연계)
- [ ] cpp e2e Track F 신설: SF-F2·F3·F7·F11을 DiscoveryRegistryHa에 구현
      (relocation-capable 노드 역할 추가, SpotActorTransfer fail-injection 패턴 이식;
      SF-F3는 별도 relocation store 컨테이너 다운, SF-F7은 chunk/budget 옵션 축소)
- [x] Bingo 재검증 (2026-08-19): node·kotlin 각 3회 클린 실행 6/6 첫 시도 그린,
      relocation 실행 확인(대체·route-ready·target_resume), 지문 재현 없음 —
      당시 미커밋 편집 트리가 원인으로 종결 (로그 scratchpad/sample-gates/*-recheck-*)
- [ ] dotnet 샘플 6종 실행 (Bingo, DeliveryDispatch, GameQuest, ShoppingMall,
      SupportChat, TicTacToe — 종료 코드 로그)
- [ ] java/kotlin 샘플 집계 게이트: SampleReleaseGateContractTest,
      CurrentManagerFakeBackendTest, FORBIDDEN_SAMPLE_PATTERN rg 스윕 (run_samples.sh 상단부)
- [ ] RelocationBehaviorConformanceTests(dotnet) batch hang 근본 원인·처분
- [ ] dotnet TicTacToe JoinGameNotify timeout(exit 134) 조용한 머신에서 재현 조사
      (샘플 게이트 1차는 PASS였음 — 재현 안 되면 종결 기록)
- [ ] harness 기본 `all` 스테이지 깨끗한 단독 재실행 (동시 에이전트 경합으로 1차
      판정불가; message-follow "Raw MeshNode requires the host Application Job Queue"
      사전 실패 주장 포함 확인)
- [ ] cpp bind-session 재시도 소진 분류(deadline_exceeded로 갱신됨, `46ef4b0f03`)의
      교차 언어 parity 확인 — java/node/dotnet의 동일 시나리오 분류 대조
- [x] cpp standalone actor 직접 relocation 복원 갭 — **판정 완료(2026-08-19): cpp
      실결함.** 4언어·스펙 모두 "타깃의 기존 Entry Spot으로 이동"이 계약인데 cpp만
      restore에 target_spot=nullopt 하드코딩(stateful_object_runtime.cpp:1584),
      드레인 경로(app.cpp:3575)가 실패, 기존 m6c:3010 테스트는 materialization
      미배선으로 거짓 그린. **수정 완료 — `aff9511de6`**(Entry Spot 로컬 해석,
      aggregate 경로 무변경, 무crash 명시 실패, disabled-fallback 증명, 41/41 ×2)
      [D에서 승격 2026-08-19]
- [ ] m6b M-c mismatched-identity rejection 테스트 (aggregate identity-fencing 조사
      포함) [D에서 승격 2026-08-19]
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
      [C] reconcile 3분기(store 조회 기반)는 spot_id 조기 캡처 판정으로 진행 중
      [발견·해소 2026-08-19]
- [ ] **sol 전 문서 spec-gap 리뷰**: 스펙·guide·e2e·언어 interface 전체 vs 구현 대조,
      gap 0 확인 (완료 조건)
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
- [ ] **C-3b descriptor payload 전-필드 규범 스키마 고정(2026-08-19 신설, 3언어
      차단 해소)**: golden의 descriptor는 예시 최소형 — node/dotnet/java가 각자 형태
      발명 중이던 것을 java 정찰이 적발. 스펙 21 §2.3 논리 계약+4언어 실제 타입에서
      규범 필드 도출→§2.4 JSON 스키마 명문화→golden 전-필드 벡터→4언어 골든 갱신.
      진행 중(최우선)
- [x] C-3 store 레코드 golden fixture — `bdc3e8a8c5`: 6키/5값 벡터(redis Lua cmsgpack 실검증), 21 §2.4 authority 스키마·22 §7 확장, 4언어 소비 테스트 즉시 그린. C-4 명시 이월: cpp authority payload hex→base64, encode측 스탠딩 테스트
- [ ] C-4 4언어 store 구현 수렴 — 진행: **키 브레이스 판정(2026-08-19): Cluster
      hashtag 리터럴 유지** — golden이 오독으로 brace-less 고정했던 것 정정
      `8a3804a110`. **java 슬라이스 1차 `326833810b`**(base64 제거=Lettuce codec
      문제였음·0x01 태그·PSETEX blob·프로덕션 encode conformance, 모듈 그린).
      **node 1차 `eb3d74f6cc`**(키·opaque 로그·blob; envelope·시퀀스는 C-4b),
      **cpp 완료 `e14bce0297`**(전면 재작성+hex→base64+encode 검증, location 7/7,
      live redis 실키 확인). dotnet 슬라이스 게이트 대기 중.
- [ ] C-4b node 마감 — terra 1차 완료(canonical envelope 5종+전역 시퀀스+strict
      recordVersion, typecheck/build/m6c 그린). 코디네이터 redis 검증: 54/55 —
      **aggregate prepare 교차 인스턴스 수렴 1건 실패(양쪽 conflict)** → terra 재투입
      수정 중. 원 항목: ① canonical JSON envelope 전환(값이 아직 구 필드 형태 —
      키는 맞으나 타 언어가 파싱 불가, location-store-repository.ts 3591줄 CAS 재작성),
      ② objectGeneration을 identity별 카운터→store 전역 시퀀스로(reserve() ~:965 —
      기존 감사의 "node는 이미 전역" 기록은 오류였음) [node 1차 eb3d74f6cc에서 이월]
- [ ] C-4c dotnet Lua byte 정합 확인: 기준 Lua의 expiresAtMs -1 센티널·정수 tombstone이
      golden(0·bool)과 상이할 가능성 — dotnet encode conformance에서 검증·수정
      [node 발견 2026-08-19]
- [ ] C-4a java 수렴 — **재정찰로 지형 재정의(2026-08-19)**: terra 감사의 Lua-HASH
      스택은 실은 **죽은 코드**(프로덕션 미도달, 자기참조+테스트 1개뿐). 라이브
      경로는 core의 ZLinkProviderDescriptorRepository/ZLinkProviderLocationRepository
      — 자체 비규범 스킴(zlink:v11: 프리픽스, PascalCase JSON, 이진 generation 접두,
      recordVersion 부재). **재범위**: ① provider 저장소를 §2.4로 포팅(node 참조
      구현, 타입별 단계·게이트·커밋), ② 죽은 Lua 스택은 oracle 커버리지 확인 후
      일괄 삭제(POSDDD), ③ 골든은 실제 writer 구동. sonnet 진행 중(커버리지 확인→
      mesh 포팅 순)(descriptor:mesh:*, owner-lease:* 등 →
      canonical-JSON-over-opaque) + ZLinkRedisAuthorityClient(3029줄) counter/CAS
      감사·수렴 — **상호 운용에 필수**(전용 경로가 남는 한 java descriptor를 타
      언어가 못 읽음), 전담 세션 규모로 분리 [java 1차에서 이월]
- [ ] C-5 cpp actorJoin 발신 연산 신설 (service-wire cross-node join 요청/응답,
      binary tail 탑재 — codec은 `7ed3992ccd`로 기구현, node 발신 경로 참조)
- [ ] C-6 dotnet actorJoin 발신 연산 신설 (BeginJoin의 local-or-NotConnected 한계 해소,
      codec은 `327b58a3d2`로 기구현)
- [ ] C-7 harness 교차 언어 relocation stage 전 쌍 그린 (JoinEntrySpot 경로 우선,
      C-5·C-6 후 일반 join 경로 — 기존 opt-in 스테이지 `2907df293f`/`c43758fc05` 기반)
- [ ] C-8 교차 언어 스테이지를 harness 기본 `all` 게이트에 편입 (회귀 구조 차단)
- [ ] C-9 상호 운용 신규 코드 sol 리뷰 + POSDDD 패스 — **1차 sol 배치 리뷰 완료
      (2026-08-19), 발견 5건 전부 배정**: [C] reconcile 기한 스윕의 relay-ready
      비가역성 위반(→판정 정정: 기한 도달 시 Location Store authority 조회로 확정
      타깃 추종/소스 복원/명시 unavailable 3분기 — 전문 에이전트), [H] authority
      실제 producer가 4언어 모두 golden 비호환(cpp 여분 storeVersion·node 구
      envelope·java 구 hash·dotnet 3키 — 각 슬라이스에 배정), [H] cold-probe 합성
      follow가 op identity/deadline/source/reply-route 4값 미보존(전문 에이전트),
      [M] cpp Lua point-read 0x01 미검증(cpp store 에이전트), [M] golden 테스트가
      실제 producer 미구동+dotnet brace-less 잔재(각 언어 마감에 편입)
- [ ] C-10 node relocationFailed row-collapse 세분화: NotFound/Rejected/InvalidOp 등을
      wire 후보(15/33/34)로 구분 인코딩 — 4언어 기준 매핑(97fc074058)과 정렬
      [D에서 편입 2026-08-19; 전용 wire 어휘 확장(ShuttingDown 등)은 이때 재판정]

## E. 확정 후속 단계 (사용자 승격 2026-08-19 — 완료 조건 포함, C 완료 후 착수)

- [ ] E-1 dotnet 성능 패스(`062c0bbd1e`에 문서화된 6건): message-follow 중복 lock
      수동 해체, ToHex 정렬 comparer, HeldRecords O(N²), 중복 participant 스캔,
      codec 삼중 복사, LINQ 재해석 캐시
- [ ] E-2 cpp complete_relocation_assembly 구조 분해 (등가성 테스트 선행 작성 후 분해)
- [ ] E-3 dotnet StageInboundSpotAggregateAsync participant-restore 추출
- [ ] E-4 cpp aggregate 오케스트레이션 reason 전파 (task_t<bool> → 분류 전달)
- [ ] E-5 E 단계 전체 sol 리뷰 + 전 게이트 재확인

## F. 별도 캠페인 (이 캠페인 범위 외 — 착수 시 별도 계획 문서)

- e2e_inventory 기존 backlog 168건: relocation과 무관한 14개 문서 전반의
  교차참조·feature-map 부채. 규모가 크고 주제가 달라 분리 (사용자 재확인 대기)

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
