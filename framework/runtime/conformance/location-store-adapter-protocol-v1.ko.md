# Location Store adapter JSONL 프로토콜 v1

공통 실행기는 case 입력, 장애 순서와 결과 판정을 소유한다. Adapter는 배포 provider의 공개
SPI를 호출하고 언어별 타입을 이 문서의 표현으로 변환한다. Case ID와 의미는
[`location-store-provider-v1.json`](location-store-provider-v1.json)이 소유한다.

## Process와 메시지

실행기는 언어 ID `node`, `dotnet`, `jvm`, `cpp`마다 명령과 인자를 설정한다. 현재 등록된
adapter는 Node다. 각 process는 UTF-8 JSONL을 stdin으로 읽고 stdout으로 응답한다. Stderr는
진단 전용이다. 한 줄은 메시지 하나이며, bytes나 문자열 안의 줄바꿈은 JSON escape로 쓴다.

요청과 응답은 `v: 1`과 문자열 `id`를 포함한다. 요청 ID는 process 안에서 유일하다. 응답은
완료 순서로 나올 수 있으며 실행기는 `id`로 연결한다. Adapter는 operation을 기다리는 동안에도
stdin을 읽어 다른 Store의 operation과 `cancel`을 처리한다. 요청마다 응답 하나를 보낸다.
Process 종료, 응답 누락, 중복 응답, 알 수 없는 ID와 잘못된 JSON은 adapter 실패다.

```json
{"v":1,"id":"c1","op":"create","store":"s1","options":{"endpoints":["http://127.0.0.1:2379"],"namespace":"example","operationTimeoutMs":5000}}
{"v":1,"id":"c1","ok":true,"result":{"kind":"created"}}
{"v":1,"id":"w1","op":"write","store":"s1","request":{"conditions":[{"kind":"missing","key":"a"}],"mutations":[{"kind":"put","key":"a","bytesBase64":"AP8=","retentionMs":"1235"}]}}
{"v":1,"id":"w1","ok":true,"result":{"kind":"applied","storeNowMs":"100000","putVersions":[{"key":"a","version":"opaque-version"}]}}
```

이 요청은 Store를 생성하고 조건부 쓰기를 실행한다. `store`는 process 안에서 유일한 instance
이름이다. 같은 process에 Store 여러 개를 만들 수 있다. Cursor는 생성한 Store instance에만
다시 전달한다.

## 값 표현

| 항목                     | JSON 표현                                                               |
| ------------------------ | ----------------------------------------------------------------------- |
| Key, prefix              | UTF-8 문자열. NUL과 Unicode를 그대로 보존한다.                          |
| Bytes                    | `bytesBase64`의 표준 base64. Padding을 포함하며 빈 bytes는 `""`다.      |
| 시각                     | Epoch milliseconds의 음이 아닌 정수 decimal 문자열.                     |
| Expiry                   | `expiresAtMs`의 decimal 문자열. 만료가 없으면 `null`이다.               |
| Retention                | `retentionMs`의 정수 milliseconds decimal 문자열. 생략하면 만료가 없다. |
| Version, cursor          | 해석하거나 정규화하지 않는 문자열. Cursor가 없으면 `null`이다.          |
| Put versions             | `{ "key": "…", "version": "…" }`의 배열. 실행기는 key로 대응한다.       |
| Limit, operation timeout | JSON 정수. Public API의 bound를 적용한다.                               |

공통 case는 retention을 정수 milliseconds로 보낸다. `1234.1ms` 같은 입력의 올림은 표현할 수
있는 언어의 native 시험에서 검사한다. Adapter가 올림을 대신 수행하지 않는다.

## Operation

| `op`      | 추가 필드                                                    | 성공 결과                                 |
| --------- | ------------------------------------------------------------ | ----------------------------------------- |
| `create`  | `store`, `options`                                           | `{ "kind": "created" }`                   |
| `read`    | `store`, `key`                                               | `found` 또는 `missing`                    |
| `write`   | `store`, `request.conditions`, `request.mutations`           | `applied` 또는 `conflict`                 |
| `scan`    | `store`, `request.prefix`, `request.limit`, `request.cursor` | `page` 또는 `expired`                     |
| `cancel`  | 진행 중인 요청 ID인 `target`                                 | `{ "kind": "cancelled", "active": true }` |
| `dispose` | `store`                                                      | `{ "kind": "disposed" }`                  |

`create.options`는 endpoint 배열, namespace, `operationTimeoutMs`를 받는다. 선택적인 `credentials`는
`user`, `password`를 받는다. 선택적인 `tls`는 `trustedCaPem`과
`clientIdentity: { certificatePem, keyPem }`를 받으며 각 언어의 공개 option으로 변환한다.
실행기는 취소된 operation을 새로 제출하지 않는다. `cancel.active`는 요청을 찾았는지만 나타내며,
그 operation의 성공이나 commit을 확정하지 않는다. 완료한 요청을 취소하면 `active: false`다.
`dispose`는 공개 수명 종료 경로를 기다린 뒤 instance 이름을 해제한다.

Condition은 `{kind: "missing", key}`, `{kind: "version", key, version}` 또는
`{kind: "value", key, bytesBase64}`다. Mutation은 `{kind: "delete", key}` 또는
`{kind: "put", key, bytesBase64, retentionMs?}`다. 실행기는 동일 key를 condition과 mutation에서
중복 사용하는 등 유효하지 않은 public 입력도 보낼 수 있다. Adapter는 이를 provider에 전달한다.

```json
{"v":1,"id":"r1","ok":true,"result":{"kind":"found","value":{"bytesBase64":"AP8=","version":"opaque-version","expiresAtMs":"101235","storeNowMs":"100000"}}}
{"v":1,"id":"r2","ok":true,"result":{"kind":"missing","storeNowMs":"101235"}}
{"v":1,"id":"w2","ok":true,"result":{"kind":"conflict","storeNowMs":"101235"}}
{"v":1,"id":"s1","ok":true,"result":{"kind":"page","value":{"storeNowMs":"100000","items":[{"key":"a","value":{"bytesBase64":"AP8=","version":"opaque-version","expiresAtMs":null,"storeNowMs":"100000"}}],"nextCursor":"opaque-cursor"}}}
{"v":1,"id":"s2","ok":true,"result":{"kind":"expired"}}
```

정상 SPI 결과는 `found`, `missing`, `applied`, `conflict`, `page`, `expired`다. Page의 각 item은
key와 found의 `value` 모양을 사용한다. 마지막 page의 `nextCursor`는 `null`이다.

## 오류와 취소

실패 응답은 `ok: false`와 `error`를 사용한다. `nativeType`과 `message`는 진단용이며 공통
판정은 category를 사용한다. Adapter가 일반 provider 오류에서 private 예외 정보를 복원하지 않는다.

```json
{"v":1,"id":"w3","ok":false,"error":{"category":"argument-error","nativeType":"RangeError","message":"invalid input"}}
{"v":1,"id":"w4","ok":false,"error":{"category":"store-failure","commit":"unknown","nativeType":"Error","message":"operation failed"}}
{"v":1,"id":"r3","ok":false,"error":{"category":"adapter-error","nativeType":"Error","message":"invalid base64"}}
```

`argument-error`는 public 입력 검증 오류다. `store-failure`는 provider operation 실패이며 `commit`은
`none` 또는 `unknown`이다. 쓰기를 시작한 뒤 실패해 commit을 확정할 수 없으면 `unknown`을 쓴다.
Read와 scan의 실패는 `none`이다. Node adapter는 일반 쓰기 실패를 보수적으로 `unknown`으로
표현한다. 변환 실패와 protocol 오류는 `adapter-error`다. Crash는 실행기가 같은 범주의 process
실패로 기록하며 `store-failure`로 바꾸지 않는다. 파싱하지 못한 요청의 응답 ID는 `null`이며
실행기는 해당 process를 실패로 종료한다.

| 언어 ID  | 공개 취소 표현                                                                                             |
| -------- | ---------------------------------------------------------------------------------------------------------- |
| `node`   | 요청별 `AbortController`와 `AbortSignal`                                                                   |
| `dotnet` | 요청별 `CancellationToken`                                                                                 |
| `jvm`    | 요청별 `ZLinkStoreCancellation`                                                                            |
| `cpp`    | SPI token이 없다. 시작 전 취소는 호출을 막으며, I/O 뒤 결과 불명은 timeout 또는 transport 단절로 검증한다. |

## Pair와 실행 결과

교차언어 case는 등록된 언어의 writer×reader 조합을 모두 실행한다. Record case는 writer의
bytes·version·expiry를 reader에서 확인한다. Version condition case는 reader가 읽은 version으로
writer가 쓰고, record 변경 뒤 같은 version의 조건이 거짓인지 확인한다. Pair마다 namespace를
분리한다. 등록된 언어가 하나면 각 교차언어 ID에 `node×node` 결과 하나가 필요하다.

```json
{
  "language": "node",
  "cases": [
    {
      "id": "etcd.cross-language-record",
      "status": "passed",
      "pairs": [{ "writer": "node", "reader": "node", "status": "passed" }]
    }
  ]
}
```

각 언어 report의 교차언어 항목은 전체 pair 결과를 포함한다. Pair 누락·중복·skip·실패는 coverage
실패다. 일반 case도 ID 누락·중복·skip·실패를 허용하지 않는다. 전체 coverage와 case 실패는 exit
status 1로 전달한다. 선택한 일부 case만 실행해도 전체 coverage를 충족하지 않으면 exit status는 1이다.

Host 시계를 바꾸는 격리 VM이 없으면 `etcd.provider-clock-independent`,
`etcd.manager-wall-clock-jumps`, `spi.snapshot-clock`은 이유와 함께 `unverified`로 기록한다.
미검증 항목은 통과 수에 포함하지 않으며 `unverified.json`에 남긴다. 기본 실행은 미검증 항목이
있으면 실패한다. 명시적인 `--allow-unverified`만 이 목록을 허용하며, 나머지 case와 pair의
coverage는 계속 요구한다. Node test wrapper의 같은 옵션은 `ZLINK_ETCD_ALLOW_UNVERIFIED=1`이다.

Mutation은 같은 case 함수를 격리 provider 산출물에 실행한다. 정확한 대상 구문이 없거나 산출물
생성에 실패하면 `not-built`다. Case assertion 실패만 `detected`로 세며, adapter 오류·crash·예상하지
않은 Store failure는 `not-detected`다. 실행하도록 설정한 mutation은 모두 `detected`여야 성공한다.
