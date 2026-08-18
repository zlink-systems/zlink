# Framework service protocol assets

이 디렉터리는 네 runtime lane이 함께 사용하는 service wire와 durable format 생성 입력의 위치다.
Application 공개 API나 공통 native runtime을 제공하지 않는다.

- `service-wire-v1.schema.json`: command, flag, field, bound와 RouteMesh·ClientServer·fanout liveness profile의 단일
  생성 입력
- `validate-service-wire-schema.mjs`: integer encoding, 재귀 aggregate length capacity, field reference,
  정렬·중복, closed union, TLV 순서, durable checksum·semantic relation과 exact relocation state rule을 확인하는 생성
  전 gate
- `golden/durable-authority-v1.json`: 네 runtime이 Ready Instance cold activation recovery pointer를
  읽고 쓰는 golden fixture
- `golden/instance-activation-recovery-v1.json`: target-owned cold activation의 source·target lifecycle,
  operation·reply·deadline, command 39 metadata presence·frame과 complete first application payload를 복원하는
  immutable envelope golden fixture
- `golden/relocation-envelope-v1.json`: 네 runtime이 Relocation Store가 단독 소유하는 non-empty saved work·timer
  bytes를 읽고 쓰는 golden fixture
- `golden/reply-relay-v1.json`: maintenance request의 terminal reply relay command 33과 exact source fence를
  닫는 ACK command 46을 네 runtime codec이 같은 bytes로 읽고 쓰는 golden fixture
- `golden/relocation-control-v1.json`: maintenance relocation의 Ready reply·post-capture ingress Data·one-way
  Cutover·payload Restore Prepare·직접 전송 state chunk command 30·31·34·40·52를 네 runtime codec이 같은
  field 순서와 bytes로 읽고 쓰는 golden fixture. Command 52 malformed 거부 사례를 함께 고정한다. 폐기한
  command 32·35·41은 fixture에 없으며 수신하지 않는다.
- `golden/session-relocation-barrier-v1.json`: exact Session seal request/reply command 42·43과 reply 없는
  commit·pre-cutover abort command 44를 고정한다. 폐기한 completion reply command 45는 fixture에 없으며 수신하지 않는다.
- `golden/authority-key-v1.json`: MeshName과 독립적인 global ActorId·SpotId를 canonical Store key로 만드는
  정상 encoding fixture
- `golden/store-record-v1.json`: Location Store opaque record(21 §2.4, 22 §7)의 key 파생(logical key
  preimage, SHA-256, Redis key 조립)과 value byte 표현(cmsgpack array + `0x01` format tag)을 mesh-node·
  owner-lease·client-server·fanout-publisher·authority record(actor·spot 각각)별로 고정하는 golden fixture. Relocation Store
  raw-bytes blob key(23 §8)도 함께 고정한다. `verify-service-wire-decoder-fixtures.mjs`가 실제 Redis Lua
  `cmsgpack.pack` 출력과 byte 단위로 대조해 만든 독립 decoder로 이 fixture를 검증한다.
- `golden/bound-session-replaced-v1.json`: command 51의 Actor authority source fence, 이전 session owner
  lifecycle·binding identity, malformed record와 재시작 전 lifecycle 거부를 고정하는 golden fixture
- `golden/framework-multipart-v1.json`: Framework 내부에서 opaque message parts를 보관하는 multipart envelope의
  정상·거부 bytes를 네 runtime이 동일하게 검증하는 golden fixture
- `golden/contract-amendment-v1.json`: object role·capacity descriptor, durable creation intent, generic
  Reserve·Commit·Abort fence, User Spot aggregate, bounded Message Follow, exact-ref route와 command 47·48 terminal
  service operation, command 51 session 교체 통지의 공통 golden fixture
- `golden/`: service frame의 정상·경계·오류 fixture를 추가하는 위치
- `generate-service-wire-assets.mjs`: 검증한 schema에서 네 언어 command·flag·Framework wire error·multipart
  profile 상수와 공통 decoder fixture를 생성하고 `--check`로 drift를 차단하는 도구
- `generated/`: C++·.NET·JVM·Node.js runtime이 복사하지 않고 사용하는 생성 상수
- `traces/`: schema 승인 뒤 생성하는 normalized behavior trace

Codec table이나 fixture를 생성하기 전에 다음 명령이 성공해야 한다. 현재 gate는 39개 command, 155개 type,
4개 flag, 33개 bound, durable fixture 4개와 logical·JSON·multipart·authority key fixture를 확인한다. `--self-test`는
contract amendment fixture 1개와 231가지 invalid mutation이 실제로
거부되는지도 확인한다. 여기에는 integer overflow, length capacity 초과, 잘못된 정렬 field, enum domain 이탈,
conditional discriminator 오류, TLV 순서·required capability 제약 변경, relocation vector 불일치, durable
magic·version·length·checksum·semantic·order·range 훼손, relocation graph·policy 오류와 fanout socket·beacon·deadline
변경이 포함된다.

`verify-service-wire-decoder-fixtures.mjs`는 위 schema self-test와 별도로 `golden/store-record-v1.json`도
검증한다 — key 파생 6건(sha256 재계산과 Redis key 조립)과 value byte 벡터 5건(cmsgpack member를 schema가 아닌
독립 decoder로 다시 decode해 원본과 byte 단위로 대조하고, JSON layer는 field 단위로 대조)이다. 이 cmsgpack
byte 벡터 자체는 실제 Redis Lua `cmsgpack.pack` 출력과 수기로 대조해 만들었다 — 그 대조는 fixture를 만들 때 한
번 수행한 수작업이며, 이 저장소의 상시 테스트는 아니다(향후 encode 방향 assertion을 추가할 여지가 있다).

Framework error는 `none=0`, 그 외에는 `wire value = public error kind + 1`로 변환한다. Service wire에서
사용하지 않는 public-only error에 대응하는 wire `23..32`는 예약 구간이다. `SpotGenerationStale`은
wire `33`에서 public `32`로, `SpotMoving`은 wire `34`에서 public `33`으로 decode한다.
`RelocationDataLost`는 wire `35`에서 public `34`로 decode하며, decoder fixture가 예약 구간과 이 변환을
함께 검증한다.

```bash
node framework/runtime/protocol/validate-service-wire-schema.mjs \
  --self-test framework/runtime/protocol/service-wire-v1.schema.json
node framework/runtime/protocol/generate-service-wire-assets.mjs
node framework/runtime/protocol/generate-service-wire-assets.mjs --check
node framework/runtime/protocol/verify-service-wire-decoder-fixtures.mjs
```

Durable fixture의 header는 4-byte magic, version `1`, zero flags, big-endian `u32` body length, exact body와
trailing CRC32C 순서다. CRC32C는 magic부터 body의 마지막 byte까지 포함한다. Location·Relocation Store provider는
이 bytes를 해석하지 않고 opaque value로 저장한다. Validator는 fixture를 semantic value로 decode한 뒤 다시
encode한 body도 원래 bytes와 일치하는지 확인한다.

현재 목표 의미와 validation 순서는
[`51-internal-service-wire-protocol.ko.md`](../../doc/framework/common/spec/server/51-internal-service-wire-protocol.ko.md)가 설명한다. 언어별 상수
파일은 검증한 schema에서 생성하며 직접 편집하지 않는다.
