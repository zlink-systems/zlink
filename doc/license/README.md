English | [한국어](README.ko.md)

# License Policy

> Which license applies to each repository layer, what each one permits, and where
> the canonical texts live.

## 1. Three tiers, one repository

| Layer | Directories | License | Canonical text |
|-------|-------------|---------|-----------------|
| Engine | `core/`, `bindings/` (per-language native bindings) | MPL-2.0 | [`/LICENSE`](../../LICENSE) |
| Framework | `framework/**` (SPOT/actor, channel messaging, STREAM, drain, location store integration) | zlink License 1.0 | [`/framework/LICENSE`](../../framework/LICENSE) |
| http-client | each language's `http-client` package under `framework/` (dotnet/java/kotlin/node/cpp) | Apache-2.0 | declared per ecosystem, see §5; full text also at [`framework/languages/cpp/http-client/LICENSE`](../../framework/languages/cpp/http-client/LICENSE) |

The split is deliberate, not accidental. Each tier answers a different
question about what it's allowed to protect and what it doesn't need to.

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

## 3. `framework` uses zlink License 1.0

`framework/` uses zlink License 1.0. The complete license text is in
[`framework/LICENSE`](../../framework/LICENSE).

Under §3.4 of that license, versions before 1.0.0 are free regardless of
machine count. Starting with 1.0.0, one server machine is free and each
additional machine requires a paid license. Already-released versions
through 0.30.0 remain under FSL-1.1-ALv2.

## 4. `http-client` uses Apache-2.0

The `http-client` package that exists under `framework/` in every language
is a thin wrapper over that platform's commonly used HTTP client library —
`System.Net.Http` (.NET BCL), `undici` (Node.js's own official HTTP client),
`java.net.http` (JDK standard library), and Boost.Beast (C++, already
vendored under `core/external/boost/`, Boost Software License 1.0). All four
are consumed as ordinary dependencies (via `PackageReference`/`dependencies`/
`api(...)`), not copied source — so, unlike `core`/libzmq, there's no
license-inheritance constraint on `http-client`'s own wrapper code at all.

Each `http-client` package uses the standard SPDX `Apache-2.0` expression,
declared per ecosystem as shown in §5. Its complete license text is included
with the C++ package and in the repository at
[`framework/languages/cpp/http-client/LICENSE`](../../framework/languages/cpp/http-client/LICENSE).

## 5. Where this is declared, per ecosystem

| Ecosystem | `framework` (zlink License 1.0) | `http-client` (Apache-2.0) |
|-----------|---------------------------------|------------------------------|
| dotnet | `Directory.Build.props` sets `PackageLicenseFile=LICENSE`; `Directory.Build.targets` bundles `framework/LICENSE` in packable `.nupkg` files | `Zlink.HttpClient.csproj` sets `PackageLicenseExpression=Apache-2.0` |
| java/kotlin | The root `build.gradle.kts` Maven `pom.licenses` block names zlink License 1.0 and links to `framework/LICENSE` | The same block declares `zlink-http-client` and `zlink-http-client-kotlin` as Apache-2.0 |
| node | Each package uses `"license": "SEE LICENSE IN LICENSE"` and includes the canonical license text in its package-root `LICENSE` file | `http-client/package.json` declares Apache-2.0 and includes its Apache license text |
| cpp | Source archives include `framework/LICENSE`; Conan packages copy it to `licenses/LICENSE`; vcpkg installs it as the copyright file and uses `"license": null`; source files use `LicenseRef-zlink-License-1.0` | `framework/languages/cpp/http-client/LICENSE` contains the Apache-2.0 text, and its source files use `SPDX-License-Identifier: Apache-2.0` |
| Unity | The package uses `"license": "Refer to LICENSE.md file"` and includes the full text in its package-root `LICENSE.md` file | — |
