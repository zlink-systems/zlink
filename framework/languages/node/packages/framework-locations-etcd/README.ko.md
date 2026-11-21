# etcd Location Store provider

이 package는 **Redis with etcd** plugin의 etcd Location Store를 제공한다. Relocation Store에는 Redis package를 함께 설치하고 별도로 등록한다. 설치와 두 등록 호출은 [Store plugin 설정](../../../../doc/framework/node/guide/server/25-location.ko.md#store-setup)을 따른다.

이 package는 etcd v3.6.16 공용 proto를 사용하는 `ZLinkEtcdLocationStore`와 Store 관리 도구를 제공한다.
Provider는 `@zlink-systems/framework`의 `ZLinkLocationStore`를 구현한다.

```ts
import { ZLinkEtcdLocationStore, type ZLinkEtcdLocationOptions } from '@zlink-systems/framework-locations-etcd';

const options: ZLinkEtcdLocationOptions = {
  endpoints: ['http://127.0.0.1:2379'],
  namespace: 'my-application',
  operationTimeoutMs: 5000
};
const store = new ZLinkEtcdLocationStore(options);
// Framework에 등록한 instance는 Framework의 수명 종료 절차에 따라 정리한다.
await store.dispose();
```

Package root는 이 class와 `ZLinkEtcdLocationOptions`, `ZLinkEtcdTlsOptions`, `ZLinkEtcdClientIdentity`,
`ZLinkEtcdCredentials` type을 내보낸다. TLS는 HTTPS endpoint에 사용하고, 사용자 인증은
`credentials: { user, password }`로 설정한다. Namespace는 UTF-8 1..64 bytes이며 NUL을 포함하지 않는다.

사용 전에 빈 namespace를 아래 `init` 명령으로 초기화하고 Store 관리 process를 운영해야 한다.
Provider의 만료 판정은 Store 시각을 사용한다. 전송한 쓰기의 결과가 유실되면 operation은 실패하며
자동으로 다시 보내지 않는다. Framework는 직접 읽기와 version 조건으로 적용 여부를 확인한다.
`dispose()`는 새 operation을 거부하고 진행 중 operation이 끝난 뒤 연결을 닫는다.

Node.js `^22.15.0 || >=23.10.0`을 사용한다. CA를 지정하지 않은 HTTPS 연결은 Node의
[`tls.getCACertificates('system')`](https://nodejs.org/download/release/latest-v22.x/docs/api/tls.html#tlsgetcacertificatestype)으로 운영체제 신뢰 저장소를 읽는다.

```sh
zlink-location-etcd-admin init --config config.json
zlink-location-etcd-admin run --config config.json
zlink-location-etcd-admin cleanup --config config.json
```

설정 형식과 종료 코드는 [공통 spec §4.4](../../../../doc/framework/common/spec/server/05-location-relocation/07-location-store-etcd.ko.md#44-store-관리-도구-명령)가 소유한다.
설정의 인증서·비밀번호 파일 경로는 설정 파일의 디렉터리를 기준으로 읽는다. 비밀번호 파일의 bytes를
UTF-8로 읽으며 마지막 개행도 비밀번호에 포함된다.

`npm run build`와 `npm pack`은 공용 proto와 원본 license를 `dist/proto/`에 복사한다. 배포물은 저장소 밖에서도
이 사본으로 동작한다. Workspace 전체 TypeScript 빌드 뒤에는 저장소의 공용 proto를 직접 읽을 수 있다.

적합성 시험은 Node workspace(`framework/languages/node`)에서 Docker를 실행한 상태로 별도로 실행한다.
장애 시험은 기본 세 번 반복하며, 60초 중지와 20초 지연 구간을 포함한다.

```powershell
$env:ZLINK_ETCD_CONFORMANCE = '1'
$env:ZLINK_ETCD_TEST_LOG_DIR = 'D:/test-logs/node-etcd'
node --test test/contract/location-etcd-conformance.test.js
```

실행기는 cluster 하나와 실제 관리 process를 사용하고, 결과·변형 시험·정리 결과를 로그 디렉터리에
기록한다. 다른 언어 provider가 필요한 두 case는 #4082에서 실행한다. 현재 공통 coverage 검사기는
35개 ID를 모두 요구하므로 이 단계의 33개 결과에는 그 두 ID의 누락을 보고한다.

## etcd client

2026-11-15 기준 저장소 선언과 잠금 상태다.

| 항목 | 현재 상태 | 근거 |
|---|---|---|
| Client | 공식 etcd v3.6.16 proto를 `@grpc/proto-loader`로 읽고 `@grpc/grpc-js`로 직접 호출한다. | [client.ts:36](src/internal/client.ts) |
| Version | 직접 dependency는 `@grpc/grpc-js` 1.14.7과 `@grpc/proto-loader` 0.8.1로 고정한다. | [package.json:22](package.json) |
| Lockfile | Workspace의 `package-lock.json`이 client와 전이 의존성의 version·integrity를 기록한다. | [package-lock.json:1](../../package-lock.json) |

Proto와 import의 원본 license는 [THIRD_PARTY_NOTICES.md:40](../../../../../THIRD_PARTY_NOTICES.md)에 기록되어 있다.

다음 목록은 위 lockfile의 두 client package에서 시작해 `dependencies`를 재귀적으로 따라간 결과다. 중첩 package 경로를 우선하므로 workspace root의 다른 protobufjs 버전과 구분한다. Framework companion package와 workspace 개발 도구는 이 client 목록에 포함하지 않는다. License 표기는 lockfile의 metadata이며 원본 license 검토를 대신하지 않는다.

| License | Client와 전이 의존성 |
|---|---|
| Apache-2.0 | `@grpc/grpc-js` 1.14.7, `@grpc/proto-loader` 0.8.1, `long` 5.3.2 |
| BSD-3-Clause | `@protobufjs/aspromise` 1.1.2, `@protobufjs/base64` 1.1.2, `@protobufjs/codegen` 2.0.5, `@protobufjs/eventemitter` 1.1.1, `@protobufjs/fetch` 1.1.1, `@protobufjs/float` 1.0.2, `@protobufjs/path` 1.1.2, `@protobufjs/pool` 1.1.0, `@protobufjs/utf8` 1.1.2, `protobufjs` 7.6.6 |
| ISC | `cliui` 8.0.1, `get-caller-file` 2.0.5, `y18n` 5.0.8, `yargs-parser` 21.1.1 |
| MIT | `@js-sdsl/ordered-map` 4.4.2, `@types/node` 22.19.19, `ansi-regex` 5.0.1, `ansi-styles` 4.3.0, `color-convert` 2.0.1, `color-name` 1.1.4, `emoji-regex` 8.0.0, `escalade` 3.2.0, `is-fullwidth-code-point` 3.0.0, `lodash.camelcase` 4.3.0, `require-directory` 2.1.1, `string-width` 4.2.3, `strip-ansi` 6.0.1, `undici-types` 6.21.0, `wrap-ansi` 7.0.0, `yargs` 17.7.3 |

[Client capability 결과](../../../../../scripts/etcd/README.ko.md#client-capability-결과)에서 시험 이름·명령·CI 원본과 미검증 범위를 확인한다.

## Client capability 검증

Node workspace에서 다음 시험을 실행한다. 크기는 payload 길이가 아니라 proto tag와 varint를 포함한
직렬화된 메시지 길이다. 송신 16 MiB와 수신 8 MiB 각각에 대해 한도 −1 byte·한도·한도 +1 byte를
검사한다. 앞의 두 메시지는 성공하고 초과 메시지는 `RESOURCE_EXHAUSTED`의 Store failure로 끝난다.
송신 초과 메시지는 서버가 받지 않으며, 수신 초과 응답을 받은 RPC도 다시 보내지 않는다.

```sh
npm run build
node --test test/contract/location-etcd-client.test.js
```

`location-etcd-security.test.js`는 실제 etcd v3.6.16에서 mTLS와 사용자 인증을 함께 검증한다.
전용 빈 namespace와 root 권한의 시험 계정을 사용한다. Fixture는 `--client-cert-auth`를 켜고
`--trusted-ca-file`, `--cert-file`, `--key-file`을 지정하며, `etcdctl user add`, `user grant-role`,
`auth enable`로 사용자 인증을 켠다. 서버 인증서는 `DNS:localhost`만 포함하고 client 인증서 CN은
etcd 사용자와 다른 이름을 사용한다. 인증서 CN으로 사용자 인증을 대체하지 않기 위해서다.

`ZLINK_ETCD_SECURITY_CONFIG`는 위 공개 option 형식의 JSON 파일 경로다. `endpoints`는
`https://localhost:<port>`이며 `tls.trustedCaPem`, `tls.clientIdentity`, `credentials`에 시험용
PEM 내용과 계정을 지정한다. `ZLINK_ETCD_WRONG_CA`는 별도로 생성한 신뢰하지 않는 CA PEM 파일이다.
시험은 namespace의 Store 시각 key와 시험 key를 기록하므로 운영 cluster에 연결하지 않는다.

```sh
ZLINK_ETCD_SECURITY_CONFIG=/path/to/test-options.json \
ZLINK_ETCD_WRONG_CA=/path/to/untrusted-ca.pem \
node --test test/contract/location-etcd-security.test.js
```

설정 파일을 지정하면 보안 시험 6개를 실행한다. 지정하지 않으면 이 시험들은 skip으로 보고한다.
성공 시험은 public provider의 read와 internal client의 revision-pinned Range를 확인한다.
`keys_only=false`는 갱신 전 binary payload를 반환하고, `true`는 같은 key·revision·version에서 빈 value를
반환한다. 최신 revision을 읽는 대조군은 갱신된 payload를 반환한다. 실패 시험은 잘못된 비밀번호,
누락된 계정, 잘못된 CA, 다른 hostname과 누락된 client identity를 각각 거부하는지 확인한다.
의존성·license의 기준은 위 client 표와 workspace lockfile이다.
