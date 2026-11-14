# d2l2 진행

- 시작: 2026-11-13T23:28:51Z, branch perf/4352-dotnet-l2-envelope, clean. 지정 문서 전체와 .NET L2 경로를 먼저 확인했다. 시간 상한 2026-11-14T01:58:51Z.
- common-rules의 BUILDS BETWEEN HOLDS를 읽었다. 이 작업은 profile 전용으로 hold를 만들지 않는다. 모든 직접 실행은 taskset6–7,14–15, raw 보존, 다른 worktree/build 미사용.
- 최초 수치: raw/historical-serial.json. serial4K L1 43976/L2 33983 제출=완료=수신, 오류0; L2 warm7623.4/count6796.4. L2 allocation 증가 source486.70B, target520.33B/message; 1K first 증가476.95/520.18B. payload 비례 추가 복사 증거 없음. G5와 단일 run 제약은 보고서에 구분한다.
- raw/alloc-before.json: request create+encode216B, request decode328B, reply create+encode216B, reply decode280B/회(타이밍 probe 고정비 약0.074B 제외). native Message wrapper 포함, native payload bytes 제외.
- 초기 build-before.log는 scratch bindings/version 경로 부재로 옛 binding0.17.3을 잘못 resolve하여 실패했다. bindings 심볼릭 링크와 production MSBuild 절대 import로 수정하고 1.25.0 resolve를 확인했다. 이 결과로 profile하지 않았다. 병렬로 시작한 own build는 종료하고 이후 production build를 순서대로 실행한다.

- profile before-L1-serial4K-warm2: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=False base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-warm2/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K-warm3: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=False base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-warm3/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K-warm4: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=False base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-warm4/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K-warm5: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=False base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-warm5/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K-v2: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-v2/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-serial4K-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-serial4K-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K-final-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-final-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K-final: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-final/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-serial4K-final-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-serial4K-final-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-serial4K-final: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-serial4K-final/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-bp1K-final-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-bp1K-final-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- before serial4K final profile 완료: L1 4138/L2 2302 제출=완료=target수신, 오류0. CPU sample/GC AllocationTick raw 보존. 모든 source/target thread가 허용 CPU 부분집합(개별 server-GC thread는6/7/14/15 한 CPU)과 SCHED_OTHER였음. warmup3s+active4s, process lifecycle15.09/14.54s. 정식 throughput 판정 아님. 10s warmup 시도는19s profile 한도와 metadata/rundown 마감이 겹쳐 폐기했다.
- 다른 작업의 exclusive hold로 analyzer/focused build가 guard에 의해 SIGSTOP됨. hold를 취득하거나 guard를 해제하지 않았다. backpressure 초기 run은 SCHED_IDLE이어서 폐기하고 raw를 보존했다.
- runtime 수정 전 소유 표·A/B/C/D·대안 대조: raw/diagnosis-before-implementation.md. golden bytes2+Unicode2는 수정 전 통과, allocation2는 실패(368→928B/header). 이전 build는 scratch/before-bin과 raw/before-built-manifest.json에 보존하여, 후속 baseline profile이 의도한 이전 소스/hash를 실행하도록 고정했다.

- profile before-L1-bp1K-final-v2-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-bp1K-final-v2-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-bp1K-w10-v3-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-bp1K-w10-v3-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-bp1K-w10-v3: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-bp1K-w10-v3/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-bp1K-w10-v3-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-bp1K-w10-v3-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-bp1K-w10-v3: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-bp1K-w10-v3/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K-v4-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-v4-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- Exact probe 전/후: request encode216→216B, request decode328→328B, reply encode216→216B, reply decode280→280B, escaped request888→328B. raw/alloc-{before,after}-exact.json. 실제 wire는 metadata가 생략되어 field13개이며 초기14개 계수(known mapping 전체 수)를 정정했다. 따라서 임시 이름 string13개/560B, 왕복 이름 UTF16 copy26→0이다.
- dotnet format 전체1280개 통과(raw/format-check.log). 연속 hold와 guard 일시정지로23분가량 걸렸으며 format 실패는 없다. Framework production DLL이 최종 source보다 새로 빌드되어 exact after probe에 연결했고 hash를 보존했다.

- profile before-L1-serial4K-v5-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-v5-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-serial4K-v5: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-serial4K-v5/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-serial4K-v5-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-serial4K-v5-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-serial4K-v5: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-serial4K-v5/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-bp1K-v5-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-bp1K-v5-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L1-bp1K-v5: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L1-bp1K-v5/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-bp1K-v5-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-bp1K-v5-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-bp1K-v5: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-bp1K-v5/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile after-L2-serial4K-v5-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/after-L2-serial4K-v5-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile after-L2-serial4K-v5: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/after-L2-serial4K-v5/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile after-L2-bp1K-v5-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/after-L2-bp1K-v5-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile after-L2-bp1K-v5: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/after-L2-bp1K-v5/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-serial4K-v6-warm: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-serial4K-v6-warm/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.

- profile before-L2-serial4K-v6: HEAD=6350275facc547759c9e143cc2515f3f528c776e dirty=True base=6350275facc547759c9e143cc2515f3f528c776e. Binary hashes: raw/before-L2-serial4K-v6/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.
