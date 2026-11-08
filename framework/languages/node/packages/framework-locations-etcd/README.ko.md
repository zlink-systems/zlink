# etcd Location Store 관리 도구

이 package는 etcd v3.6.16 공용 proto를 사용하는 Store 관리 도구를 제공한다. Location Store SPI 구현과
공개 provider 선언은 후속 단계에 포함된다. 내부 호출 계층과 갱신 engine은 package root에서 내보내지 않는다.

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
