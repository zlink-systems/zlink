# framework-locations-etcd

This package supplies the etcd Location Store for **Redis with etcd**. Install the Redis package for the separately registered Relocation Store. See [Store plugin setup](../../../../doc/framework/node/guide/server/25-location.en.md#store-setup) for installation and both registration calls.

## etcd client

Repository declarations and lock state as of 2026-11-15.

| Item | Current state | Evidence |
|---|---|---|
| Client | Loads the official etcd v3.6.16 protos with `@grpc/proto-loader` and calls them directly through `@grpc/grpc-js`. | [client.ts:36](src/internal/client.ts) |
| Version | Direct dependencies are pinned to `@grpc/grpc-js` 1.14.7 and `@grpc/proto-loader` 0.8.1. | [package.json:22](package.json) |
| Lockfile | The workspace `package-lock.json` records client and transitive dependency versions and integrity hashes. | [package-lock.json:1](../../package-lock.json) |

Original licenses for the protos and their imports are listed in [THIRD_PARTY_NOTICES.md:40](../../../../../THIRD_PARTY_NOTICES.md).

The following inventory recursively follows `dependencies` from the two client packages in the lockfile. Nested package paths take precedence, distinguishing the client protobufjs version from the workspace root version. Framework companion packages and workspace development tools are outside this client inventory. License labels are lockfile metadata and do not replace review of the original license texts.

| License | Client and transitive dependencies |
|---|---|
| Apache-2.0 | `@grpc/grpc-js` 1.14.7, `@grpc/proto-loader` 0.8.1, `long` 5.3.2 |
| BSD-3-Clause | `@protobufjs/aspromise` 1.1.2, `@protobufjs/base64` 1.1.2, `@protobufjs/codegen` 2.0.5, `@protobufjs/eventemitter` 1.1.1, `@protobufjs/fetch` 1.1.1, `@protobufjs/float` 1.0.2, `@protobufjs/path` 1.1.2, `@protobufjs/pool` 1.1.0, `@protobufjs/utf8` 1.1.2, `protobufjs` 7.6.6 |
| ISC | `cliui` 8.0.1, `get-caller-file` 2.0.5, `y18n` 5.0.8, `yargs-parser` 21.1.1 |
| MIT | `@js-sdsl/ordered-map` 4.4.2, `@types/node` 22.19.19, `ansi-regex` 5.0.1, `ansi-styles` 4.3.0, `color-convert` 2.0.1, `color-name` 1.1.4, `emoji-regex` 8.0.0, `escalade` 3.2.0, `is-fullwidth-code-point` 3.0.0, `lodash.camelcase` 4.3.0, `require-directory` 2.1.1, `string-width` 4.2.3, `strip-ansi` 6.0.1, `undici-types` 6.21.0, `wrap-ansi` 7.0.0, `yargs` 17.7.3 |

[Client capability results](../../../../../scripts/etcd/README.en.md#client-capability-results) records test names, commands, CI sources and unverified scope.
