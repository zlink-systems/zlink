# 10. 개정 후보 (비계약)

> [공통 계약 목차](README.ko.md)
>
> **이 장의 항목은 계약이 아니다.** 구현 근거가 되지 않으며, 승격이 결정되면
> 해당 장의 계약 본문으로 옮기고 5개 언어를 함께 갱신한다(README 변경 절차).

| ID | 제목 | 동기 | 결정할 것 |
| --- | --- | --- | --- |
| R1 | typed 실패 시 에러 바디 노출 | `submit<T>()`가 status ≥ 400에서 응답 body를 버려 API 에러 페이로드를 읽으려면 `submitRaw()` 우회가 필요 — 실무 함정 | 실패 값에 status+headers+raw body를 싣는 형태(예외 필드 vs 실패 봉투), 5개 언어 표현 |
| R3′ | retry 총 데드라인 옵션 | (R3의 백오프+지터는 2026-07-12 승격·구현 완료 — 6장 §6.2) 재시도 전체를 아우르는 총 데드라인은 여전히 계약에 없음(cpp만 두 경로 모두 총 예산 강제 — 언어 편차) | 총 데드라인 옵션(예: `totalTimeout`) 도입 여부, cpp 편차와의 통일 방향 |
| R4 | multipart 바이너리 파일 | `multipartFile` content가 문자열이라 바이너리 업로드 불가 | 바이트 인자 오버로드 추가 vs 파일 경로 인자, 5개 언어 시그니처 |
| R5 | kotlin coroutine 심화 | 취소가 하부 요청에 전파되지 않고, 스트리밍이 콜백 sink뿐(`Flow` 부재), java blocking `fetch`와 kotlin suspend `fetch` 동명이의 | `suspendCancellableCoroutine` 전파 범위, `Flow<ByteArray>` 다운로드 추가 여부, `fetch` 명칭 정리 |
| R7 | one-shot verb 경로 재검토 | one-shot이 요청마다 전송 스택을 생성/파괴(dotnet은 핸들러 재생성 → 소켓 고갈 위험). client/builder verb 7종 중복도 이 경로 때문 | 유지+경고 문서화 vs 내부 공유 전송 재사용 vs 제거 |
| R8 | cpp 진짜 async I/O 전환 | 현행은 동기 Beast를 스레드풀로 오프로드(sync-over-threadpool) — 워커가 요청 기간 내내 점유되고 기본 스케줄러는 직렬화 | Beast async 교환 전환 범위/일정(대형), 단기 완화(스레드 수·스케줄러 분리)와의 관계 |
| R9 | 요청 취소 표면 통일 | dotnet만 `CancellationToken`을 받고 cpp/java/node는 in-flight 요청을 취소할 수단이 없음(timeout이 유일한 경계). kotlin 취소 미전파(R5)와 같은 뿌리 | 언어 관용 취소 수단(node `AbortSignal`, java future cancel, cpp 취소 토큰) 노출 범위, 취소 시 에러 kind |
| R10 | 관찰성 훅(interceptor) | 요청/응답 interceptor가 없어 토큰 갱신·공통 로깅·zlink flow-tracing 헤더 전파·metrics를 끼울 곳이 없음. framework 본체(message-flow tracing)와의 통일성 최대 갭 | 훅 지점(요청 전/응답 후/실패), 시그니처, flow-correlation 헤더 표준 |
| R11 | 다중값 응답 헤더 | 응답 헤더가 `map<string,string>`이라 동일 name 반복(`Set-Cookie` 등)이 붕괴됨. cookie jar는 내부 처리하나 raw 소비자는 정보 유실 | `headers` 타입 확장(다중값 접근자 추가 vs map 타입 변경 — 호환성), 5언어 표현 |
| R12 | 스트리밍 표면 언어 관용화 | 다운로드 sink/업로드 provider가 동기 콜백 — backpressure 없음, 언어 관용(node async iterator, dotnet `IAsyncEnumerable`, kotlin `Flow`)과 불일치. R5의 일반화 | 콜백 병행 유지 여부, 언어별 관용 타입 매핑, cpp 대응물 |
| R13 | 호스팅/DI 통합 헬퍼 | ASP.NET Core DI·NestJS module·Spring bean 등록 헬퍼가 없어 framework 호스팅 가이드 흐름과 접점 없음. client 수명 규칙(§2.4)을 DI 컨테이너가 관리하는 게 자연스러움 | 별도 패키지 vs 본체 포함, 언어별 범위(우선 dotnet/node), 설정 바인딩 형태 |
| R14 | 조건 폴링 terminal (`poll`) | 작업 완료 대기·상태 확인 등 "응답이 조건을 만족할 때까지 간격 X, 최대 N회/기간 T 반복" 수요. 무조건 반복(loop)은 스케줄링 관심사라 transport 계약 밖이 기본 입장이나, poll의 특수형으로 포섭 가능 | terminal 시그니처(조건 술어·간격·상한), 반환 형태(최종 응답 vs 이력), retry(6장)와의 경계, 취소(R9) 선행 여부, 무조건 반복 포함 여부 |

등재/승격 이력은 이 표에 열을 늘리지 말고 plan(진행 중) 또는 커밋 메시지로
남긴다. **결번**: R2(timeout `DeadlineExceeded`)·R3(백오프+지터)·R6(헤더 대소문자)은 계약 본문
(6장 §6.2 / 4장 §4.3)으로 승격되어 제거됐다. R3의 잔여 쟁점은 R3′로 분리.
