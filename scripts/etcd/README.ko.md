# etcd provider 시험 인프라

Node.js 22의 표준 라이브러리만 사용한다. Provider와 Store 관리 process는 이 디렉터리에
구현하지 않는다. 하니스는 단일 개발 머신의 기능·장애 시험용이며 운영 배치의 host 분리를
검증하지 않는다.

## Cluster 명령

```sh
node scripts/etcd/etcd-cluster.mjs up --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs status --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs kill --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs resume --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs pause --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs resume --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs partition --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs heal --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs compact --run-dir scripts/etcd/.runs/example --revision 10
node scripts/etcd/etcd-cluster.mjs down --run-dir scripts/etcd/.runs/example
```

`up`은 새 run directory를 요구한다. Member는 1부터 세며 기본 3개다. `--members 5`와
`--quota-bytes N`을 지원한다. `resume`은 pause한 member를 재개하고 kill한 member는 같은
data directory로 재시작한다. `heal`은 이 run의 모든 peer partition을 해제한다.
`cluster.json`에는 schema version, run ID, 고정 버전, initial cluster token, decimal cluster ID,
client endpoint 목록과 member별 name·포트·data directory·log·member ID를 기록한다.
Docker는 container ID, native는 PID와 `/proc`의 process 시작 시각도 기록한다.

Linux와 WSL은 공식 바이너리를 사용한다. `etcd-version.json`의 SHA-256을 확인하고
`scripts/etcd/.cache/`에 archive를 보관한다. 이 cache는 run directory 밖에 있어야 한다.
`tar`가 필요하며 Linux x64와 arm64를 지원한다. 체크섬 불일치는 실패다.
Windows와 macOS는 Docker의 Linux container를 사용한다. `--backend docker` 또는
`--backend native`로 경로를 명시할 수 있다. Docker는 공식 이미지를 digest로 고정하며
run마다 client·peer network 두 개를 만든다. `partition`은 peer network만 끊는다.
Native는 etcd Raft HTTP의 source member ID를 읽는 peer 전용 proxy로 양방향 연결을 끊는다.
기존 연결도 종료하며 client RPC에는 관여하지 않는다. 이 경로는 고정한 etcd 버전에 맞춘 시험 도구다.

Docker의 client host 포트는 Docker가 배정하며 peer는 Docker network만 사용한다.
Kill한 Docker member를 재시작하면 host 포트가 바뀔 수 있으므로 `cluster.json`의 endpoint를 다시 읽는다.
Native는 `.runs/.ports/`의 포트별 배타 lock에 run ID를 기록하고 `down`에서 자기 lock만 지운다.
포트를 고를 때 이미 사용 중인 포트는 건너뛴다. 고른 뒤 etcd가 bind하기 전에 하니스 밖 process가
같은 포트를 쓰는 경쟁은 막지 못하며, 그 bind 실패는 포트 사용 원인을 보고하고 재시도하지 않는다.

준비 확인은 모든 member의 cluster ID 일치, linearizable read와 Txn 성공을 요구한다.
서버는 spec §9.2의 transaction·요청 크기와 compaction 하한을 사용한다.
포트 충돌이나 준비 실패는 오류로 끝나고 이 run의 자원만 정리한다. `down`은 member log와
data directory를 보존한다. Docker 정리는 run label로, native signal은 PID와 시작 시각으로
대상을 확인한다. `.cluster.lock`은 같은 run의 동시 명령을 거부한다. 강제 종료로 lock만
남았다면 해당 run 명령이 실행 중이지 않은지 확인한 뒤 lock을 지우고 `down`을 실행한다.

## RPC 보류

```sh
node scripts/etcd/hold-proxy.mjs serve --target http://127.0.0.1:2379 --state-file scripts/etcd/.runs/proxy.json
node scripts/etcd/hold-proxy.mjs hold-downstream --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs hold-upstream --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs release --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs disconnect --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs status --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs stop --control http://127.0.0.1:CONTROL_PORT
```

`serve`는 proxy endpoint·control URL·PID를 출력하고 파일에도 기록한다. `--listen PORT`와
`--control PORT`를 생략하면 OS가 포트를 배정한다. 제어 listener는 loopback에만 열린다.
`hold-upstream`은 client→server, `hold-downstream`은 server→client bytes를 보류한다.
현재 연결과 새 연결 모두에 적용한다. `release`는 양쪽을 해제하며 `disconnect`는 현재 연결을
끊는다. 제어 응답이 돌아오면 해당 동작이 적용된 상태다. TCP backpressure로 bytes를
보류하므로 JavaScript 배열에 bytes를 계속 쌓지 않는다. gRPC나 Txn 내용은 해석하지 않는다.

## Raw KV

```sh
node scripts/etcd/raw-kv.mjs put --endpoint http://127.0.0.1:2379 --key-hex 006b --value-hex 00ff
node scripts/etcd/raw-kv.mjs range --endpoint http://127.0.0.1:2379 --key-hex 006b --revision 2
node scripts/etcd/raw-kv.mjs range --endpoint http://127.0.0.1:2379 --key-hex 00 --end-hex 01 --limit 1000
node scripts/etcd/raw-kv.mjs delete --endpoint http://127.0.0.1:2379 --key-hex 006b
node scripts/etcd/raw-kv.mjs txn --endpoint http://127.0.0.1:2379 --body-file scripts/etcd/.runs/txn.json
```

Key와 value 입력은 hex다. Range는 gateway 응답과 `bytes` 배열의 `keyHex`·`valueHex`를
출력한다. Revision은 64-bit decimal 문자열로 전달한다. `--end-hex`는 range와 delete의
exclusive end다. Txn 파일은 etcd v3 gateway JSON이며 byte 필드는 base64다.
Metadata·payload를 서로 다른 Txn으로 쓰면 `mod_revision` 손상 fixture를 만들 수 있다.
도구는 잘못된 저장 형식도 그대로 기록한다. Gateway 오류는 실패 종료로 반환한다.

## Fixture와 검증

```sh
node scripts/etcd/generate-fixtures.mjs
node scripts/etcd/validate-fixtures.mjs
node framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs
node --test scripts/etcd/self-test.mjs
node scripts/etcd/verify-harness.mjs --backend docker --log-dir PATH
node scripts/etcd/verify-harness.mjs --backend native --log-dir PATH
node scripts/etcd/check-conformance-coverage.mjs cpp.json dotnet.json jvm.json node.json
node scripts/etcd/check-conformance-coverage.mjs --language node node.json
```

Golden은 namespace·Framework key bytes, metadata, clock, version과 손상 record를 고정한다.
기존 store record fixture의 preimage bytes를 불투명 key 입력으로 재사용하며 의미를 해석하지 않는다.
적합성 목록은 etcd §12의 25항목과 SPI §10의 provider 공통 10항목을 등록한다. Redis 전용
4항목은 원문 절과 적용 제외 근거를 기록한다. 적합성 목록 JSON은 사람이 관리하며 생성기는 golden byte만 만든다.
Validator는 `{ file, section }`과 bullet 원문을 대조하고 각 bullet의 등록이 정확히 하나인지 확인한다.
`validate-fixtures.mjs --etcd-spec PATH --spi-spec PATH`로 spec 사본을 검사할 수 있다.
저장 시각의 정상 범위는 0..9,223,372,036,854 ms이며 상한 초과와 잘못된 clock 길이는 손상 벡터다.
Store 관리 process가 필요한 case는 실제 관리 process를 시험 adapter가 실행해야 한다.
하니스는 시각 key를 자동으로 만들거나 갱신하지 않는다.

언어별 결과는 다음 JSON 형식이다. 모든 case ID를 한 번씩 `passed`로 제출해야 하며
누락·skip·실패·중복·알 수 없는 ID는 실패다. 기본 검사는 네 언어 모두를 요구한다.

```json
{"language":"node","cases":[{"id":"etcd.cross-language-record","status":"passed"}]}
```

`self-test.mjs`는 가짜 통과 결과의 성공과 누락·skip 결과의 실패, byte 보류·해제와 raw KV
gateway 처리를 확인한다. `verify-harness.mjs`는 두 run을 동시에 띄우고 kill·재시작·pause·peer
partition·heal·compaction과 raw KV를 확인한 뒤 모두 정리한다. 명령별 출력, member log,
manifest와 cleanup 결과를 log directory에 남긴다. `--runs 4 --isolation-only`는 네 run의 격리와 정리만 검사한다.
`--runs 1`은 cluster 하나로 lifecycle을 검사한다. 이 검증은 provider 적합성 실행 결과가 아니다.
