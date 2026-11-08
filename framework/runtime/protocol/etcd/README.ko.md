# etcd v3.6.16 공용 proto 사본

공식 etcd Location Store provider와 Store 관리 도구는 이 디렉터리의 proto 사본을 함께 사용한다.
파일은 upstream bytes 그대로 보관하며 import 경로를 바꾸지 않는다. Proto 검색 경로는 `v3.6.16/`이다.
진입 파일은 `v3.6.16/etcd/api/etcdserverpb/rpc.proto`다.

etcd proto는 `etcd/api/` import 경로에 맞춰 배치했다. 의존 버전은 etcd v3.6.16의
[`api/go.mod`](https://github.com/etcd-io/etcd/blob/v3.6.16/api/go.mod)와
[`scripts/genproto.sh`](https://github.com/etcd-io/etcd/blob/v3.6.16/scripts/genproto.sh)의
`googleapi_commit`을 기준으로 한다.

`google/protobuf/descriptor.proto`와 `google/protobuf/struct.proto`는 protobuf well-known type이다.
Node의 `@grpc/proto-loader`는 `protobufjs`에 포함된 해당 정의를 사용하므로 별도 사본을 두지 않는다.
다른 언어의 proto compiler에는 해당 compiler가 제공하는 well-known type 검색 경로를 추가한다.

## 파일 출처와 SHA-256

경로는 `v3.6.16/` 기준이다. SHA-256은 내려받은 파일 bytes로 계산했다.

| 파일 | 출처 URL | 태그·commit | SHA-256 |
|---|---|---|---|
| `etcd/api/etcdserverpb/rpc.proto` | [upstream](https://raw.githubusercontent.com/etcd-io/etcd/v3.6.16/api/etcdserverpb/rpc.proto) | `etcd v3.6.16` | `c57117e8f4764667be927d862f4fb01fbf1faaf0f725bf2eecc3a523e12aacc6` |
| `etcd/api/mvccpb/kv.proto` | [upstream](https://raw.githubusercontent.com/etcd-io/etcd/v3.6.16/api/mvccpb/kv.proto) | `etcd v3.6.16` | `7416acaac7c76524c717df9107a8020d3c4eda92a490a35f1f211592ce8cfa1d` |
| `etcd/api/authpb/auth.proto` | [upstream](https://raw.githubusercontent.com/etcd-io/etcd/v3.6.16/api/authpb/auth.proto) | `etcd v3.6.16` | `e2b6936f3aee307f0f183c99613983e7d5d66e5daaf6da3cd2f4b6976db0a0a1` |
| `etcd/api/versionpb/version.proto` | [upstream](https://raw.githubusercontent.com/etcd-io/etcd/v3.6.16/api/versionpb/version.proto) | `etcd v3.6.16` | `8e948ec573b1c7803d72ee54763d80ead0287e0fc6987c37293b20d3a23d0dbd` |
| `gogoproto/gogo.proto` | [upstream](https://raw.githubusercontent.com/gogo/protobuf/v1.3.2/gogoproto/gogo.proto) | `gogo/protobuf v1.3.2` | `f2f77edf7de807ded7884813d851656f4ccb18262db717a8f31061995f3e7324` |
| `google/api/annotations.proto` | [upstream](https://raw.githubusercontent.com/googleapis/googleapis/0adf469dcd7822bf5bc058a7b0217f5558a75643/google/api/annotations.proto) | `googleapis 0adf469dcd7822bf5bc058a7b0217f5558a75643` | `d3d3b4bbe80c0481c35507376f5653fcfdfcbdc7f168aab097f25ae643b7ebe8` |
| `google/api/http.proto` | [upstream](https://raw.githubusercontent.com/googleapis/googleapis/0adf469dcd7822bf5bc058a7b0217f5558a75643/google/api/http.proto) | `googleapis 0adf469dcd7822bf5bc058a7b0217f5558a75643` | `2ffe191f209a9a68e30f0493c66d5ce15be8deac276d55bcd4127f8cc7f453e2` |
| `protoc-gen-openapiv2/options/annotations.proto` | [upstream](https://raw.githubusercontent.com/grpc-ecosystem/grpc-gateway/v2.29.0/protoc-gen-openapiv2/options/annotations.proto) | `grpc-gateway v2.29.0` | `1f8e23ce506c358349d5ce0e54c336e3b2f3bf17c5d9e7d965e24a6e3429c2c9` |
| `protoc-gen-openapiv2/options/openapiv2.proto` | [upstream](https://raw.githubusercontent.com/grpc-ecosystem/grpc-gateway/v2.29.0/protoc-gen-openapiv2/options/openapiv2.proto) | `grpc-gateway v2.29.0` | `8e9c28731f17e4608a1f7457cdd6f9a2718add4f5f9a0390dd5514a88e2023ef` |
| `licenses/etcd-LICENSE` | [upstream](https://raw.githubusercontent.com/etcd-io/etcd/v3.6.16/LICENSE) | `etcd v3.6.16` | `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` |
| `licenses/gogo-protobuf-LICENSE` | [upstream](https://raw.githubusercontent.com/gogo/protobuf/v1.3.2/LICENSE) | `gogo/protobuf v1.3.2` | `3f571768c0ba2f330e5fee90de89bfe5141f48c1939be8008fa61461c30ca07f` |
| `licenses/googleapis-LICENSE` | [upstream](https://raw.githubusercontent.com/googleapis/googleapis/0adf469dcd7822bf5bc058a7b0217f5558a75643/LICENSE) | `googleapis 0adf469dcd7822bf5bc058a7b0217f5558a75643` | `c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4` |
| `licenses/grpc-gateway-LICENSE` | [upstream](https://raw.githubusercontent.com/grpc-ecosystem/grpc-gateway/v2.29.0/LICENSE) | `grpc-gateway v2.29.0` | `a15b1d1b168954c92ff7fb1620382418f7c72f4f4d251ee791d1098ad68ab0c4` |

## License

etcd, googleapis와 grpc-gateway는 Apache License 2.0, gogo/protobuf는 BSD 3-Clause License로 배포된다.
원본 license는 `v3.6.16/licenses/`에 보관한다. 각 proto의 copyright·license 주석도 원본 그대로 유지한다.
