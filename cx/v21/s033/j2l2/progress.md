# Java L2 header 작업

- 시작 branch: `perf/4352-java-l2-header`, HEAD `2125999e62`, 변경 없음. 지정 branch에서 local commit만 수행한다.
- prior `l2bp`, `lad-java-l2e`, `n2net` 보고서 확인. scratch bench는 `f3e3933567` archive이며 composite build는 이 worktree Java Framework만 참조한다.
- 프로파일은 CPU 6-7,14-15, 직접 measure.lock 사용 없음. 테스트만 지정 test-slot으로 실행한다. raw 삭제 없음.

| 단계 | Java 작업·file:line | .NET 작업·file:line | 추가 할당·복사·collection | cold JIT / steady | 변경 후보 |
|---|---|---|---|---|---|
| client request encode | Envelope create:155, writeHeader:438, encodeHeaderBytesUncached:568 | ClientCallCodec:7, EnvelopeCodec:188,1195 | Header, correlation String, byte[]; writer size+write 순회 | 수집 예정 | 준비된 값/필드 처리 |
| target request parse | Envelope decodeHeader:846, readCanonical:1213, string:1311 | EnvelopeCodec:377,414,933 | HeaderReader, CanonicalReader, String+byte[], native scratch; canonical miss 시 Jackson 재시도 | 수집 예정 | reader 비용 |
| target reply encode | DispatchReporter:378, Envelope reply:181, writeHeader:438 | ReplyWriter:34, EnvelopeCodec:1195 | reply Header, byte[]; prepared names 3회 비교 | 수집 예정 | writer 반복 |
| client reply parse/completion | CallRuntime:260, Envelope tryDecodeHeader:693; binding CompletionOwner | ClientCallCodec:60; binding CompletionOwner | parse 위와 동일; binding pending lookup/remove가 physical correlation 소유 | 수집 예정 | parser 비용만 변경 |

비교할 항목: field/value scan, Strings, byte[]/char[] copies, metadata map, cache lookup, exceptions, correlation completion, JIT tier. wire·검증·buffer 소유 결정 위치의 전후 수를 기록한다.
