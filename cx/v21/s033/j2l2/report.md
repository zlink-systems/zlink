# j2l2 중간 보고 (02:34 UTC, 감독자 작성 — job 진행 중, progress.md 요약)

- 원인(.NET 대조): Java header writer가 heap byte[]에 쓴 뒤 Message.from으로 옮겨 Message.allocate 대비 416 B/header를 더 할당한다(.NET EnvelopeCodec:1195-1206은 최종 Message span에 직접 기록). correlation 문자열 생성 184 B/header. header parser가 canonical reader + Jackson 두 개이고 검증이 겹친다(.NET은 reader 하나).
- 수정 상태(커밋 전, worktree 변경 5파일): native 최종 Message에 직접 쓰는 writer 1개, 단일 byte reader로 parser 2→1, correlation은 HexFormat 1회로 104 B/header. 할당 실패 시험은 수정 전 실패 로그 보존(raw/tests-writer-before.log, raw/tests-correlation-before.log).
- 첫 통합 reader 후보는 parse CPU가 source 1.34→3.07, target 1.72→3.57 µs로 늘어 버렸다(raw/after-final-L2-bp). cursor 처리와 field lookup(길이별 grammar table, .NET ReadHeaderField:637 방식)을 고치는 중.
- 판정 위치: header parser 2→1, 나머지 1→1(계획).
