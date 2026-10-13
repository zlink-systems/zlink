English | [한국어](README.ko.md)

# License Policy

> Which license applies to each repository layer, what each one permits, and where
> the canonical texts live.

## 1. License boundaries in the repository

| Layer | Directories | License | Canonical text |
|-------|-------------|---------|-----------------|
| Engine | `core/`, `bindings/` (per-language native bindings) | MPL-2.0 | [`/LICENSE`](../../LICENSE) |
| Server Framework runtime | Server runtime in `framework/` and packages that specify zlink License 1.0 | zlink License 1.0 | [English canonical text](../../framework/LICENSE) · [Korean translation](../../framework/LICENSE.ko.md) |
| Client connectors and copied examples | Per-language client connectors; shared C++ helper headers and the Node `stream-wire` codec used by connectors and the Framework runtime; example code under `samples/`, `tutorial/`, and `quickstart/` | Apache-2.0 | See §6 for ecosystem declarations; full text at [`framework/languages/cpp/http-client/LICENSE`](../../framework/languages/cpp/http-client/LICENSE) |
| `http-client` | Each language's `http-client` package under `framework/` (dotnet/java/kotlin/node/cpp) | Apache-2.0 | See §6 for ecosystem declarations; full text also at [`framework/languages/cpp/http-client/LICENSE`](../../framework/languages/cpp/http-client/LICENSE) |

Tests and performance-gate source retain the Framework license. Each scope has
its own redistribution terms.

## 2. Why `core`/`bindings` are MPL-2.0

`core` began as a fork of [libzmq](https://github.com/zeromq/libzmq) v4.3.5
(see `core/doc/guide/design-rationale.ko.md` for what was narrowed and why).
Separately, as public upstream history: v4.3.5 is the exact libzmq release at
which the libzmq project itself completed relicensing from LGPLv3 +
static-linking exception to MPL-2.0. `core`'s license is inherited from that
lineage: MPL-2.0 files remain MPL-2.0, and this project has no unilateral
right to relicense someone else's copyrighted code to something more
restrictive.

MPL-2.0 is a **file-level weak copyleft**. It requires MPL-covered files to
stay available under MPL-2.0, but it explicitly permits combining those
files with differently-licensed code in a "Larger Work" — this is what lets
`framework` (zlink License 1.0) link against `core`/`bindings` (MPL-2.0) without the whole
combined product having to be MPL-2.0.

`bindings/` (the per-language native binding layer that wraps `core`) stays
MPL-2.0 alongside it: it's infrastructure with no independent product/SaaS
surface of its own, so keeping it maximally permissive maximizes adoption at
zero strategic cost.

## 3. Server Framework runtime uses zlink License 1.0

The server Framework runtime and packages that specify zlink License 1.0 use
this license. The complete text is in
[`framework/LICENSE`](../../framework/LICENSE).

Under §3.4 of that license, versions before 1.0.0 are free regardless of
machine count. Starting with 1.0.0, one server machine is free and each
additional machine requires a paid license. Already-released versions
through 0.30.0 remain under FSL-1.1-ALv2.

## 4. Client connectors and examples use Apache-2.0

Client connectors and example code copied from `samples/`, `tutorial/`, and
`quickstart/` use Apache-2.0. Package metadata and source SPDX identifiers
express this boundary. Tests and performance-gate source retain the Framework
license even when they use a connector.

The Node `stream-wire` package is a shared wire-codec dependency used by both
the connector and server Framework packages. Its package metadata and bundled
license use Apache-2.0. The four C++ common headers used by the connector are
also Apache-2.0. Server Framework runtime and test source retain zlink License
1.0.

## 5. `http-client` uses Apache-2.0

The `http-client` package that exists under `framework/` in every language
is a thin wrapper over that platform's commonly used HTTP client library —
`System.Net.Http` (.NET BCL), `undici` (Node.js's own official HTTP client),
`java.net.http` (JDK standard library), and Boost.Beast (C++, already
vendored under `core/external/boost/`, Boost Software License 1.0). All four
are consumed as ordinary dependencies (via `PackageReference`/`dependencies`/
`api(...)`), not copied source — so, unlike `core`/libzmq, there's no
license-inheritance constraint on `http-client`'s own wrapper code at all.

Each `http-client` package uses the standard SPDX `Apache-2.0` expression,
declared per ecosystem as shown in §6. Its complete license text is included
with the C++ package and in the repository at
[`framework/languages/cpp/http-client/LICENSE`](../../framework/languages/cpp/http-client/LICENSE).

## 6. Where this is declared, per ecosystem

| Ecosystem | Server Framework runtime (zlink License 1.0) | Client connectors and examples (Apache-2.0) | `http-client` (Apache-2.0) |
|-----------|-------------------------------------------|---------------------------------------------|------------------------------|
| dotnet | `Directory.Build.targets` bundles `framework/LICENSE` in packable `.nupkg` files | Connector `.csproj` declares Apache-2.0 and includes the full text as `LICENSE` | `Zlink.HttpClient.csproj` declares Apache-2.0 |
| java/kotlin | The root `build.gradle.kts` Maven `pom.licenses` block names zlink License 1.0 and links to `framework/LICENSE` | The same block declares `zlink-stream-connector` as Apache-2.0; its shared `zlink-framework-json-internal` dependency remains zlink License 1.0 | The same block declares `zlink-http-client` and `zlink-http-client-kotlin` as Apache-2.0 |
| node | Server packages use `"license": "SEE LICENSE IN LICENSE"` and include `framework/LICENSE` in their package-root `LICENSE` file | `stream-connector` declares Apache-2.0 and includes the full text; `stream-wire` does too and is shared with server Framework packages | `http-client/package.json` declares Apache-2.0 and includes the full text |
| cpp | Framework packages use zlink License 1.0; server source files use `LicenseRef-zlink-License-1.0` | Conan/vcpkg metadata declare Apache-2.0; CMake installs `cpp/http-client/LICENSE`; production connector, its four common helper headers, and sample source headers use Apache-2.0 | `framework/languages/cpp/http-client/LICENSE` contains the full text and source files use `SPDX-License-Identifier: Apache-2.0` |
| Unity | — | `package.json` declares Apache-2.0 and includes the full text in package-root `LICENSE.md` | — |
