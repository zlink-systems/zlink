# etcd Location Store provider

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
