English | [한국어](README.ko.md)

# License Policy

> Why this repository uses three licenses, what each one permits, and where
> the canonical texts live.

## 1. Three tiers, one repository

| Layer | Directories | License | Canonical text |
|-------|-------------|---------|-----------------|
| Engine | `core/`, `bindings/` (per-language native bindings) | MPL-2.0 | [`/LICENSE`](../../LICENSE) |
| Framework | `framework/**` (SPOT/actor, channel messaging, STREAM, drain, location store integration) | FSL-1.1-ALv2 (Functional Source License) | [`/framework/LICENSE`](../../framework/LICENSE) |
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
`framework` (FSL) link against `core`/`bindings` (MPL-2.0) without the whole
combined product having to be MPL-2.0.

`bindings/` (the per-language native binding layer that wraps `core`) stays
MPL-2.0 alongside it: it's infrastructure with no independent product/SaaS
surface of its own, so keeping it maximally permissive maximizes adoption at
zero strategic cost.

## 3. Why `framework` is FSL-1.1-ALv2

`framework` is where the differentiated, original value lives — SPOT/actor,
channel messaging, STREAM, graceful drain, location-store-based topology.
This is the layer someone could plausibly stand up as a competing hosted
service ("ZLink Framework Cloud"). The goal was: stay open enough for wide
adoption, but capture value if that specific scenario happens.

**What FSL actually restricts.** Nothing about normal use. You may use,
copy, modify, create derivative works of, and redistribute the Software for
any Permitted Purpose — which is defined as *any purpose other than a
Competing Use*. A Competing Use means offering the Software to others as a
commercial product or service that substitutes for it or for what we
already offer using it, or that offers substantially similar functionality
to it — not just standing up a literal hosted clone. `framework/LICENSE`
explicitly lists four Permitted Purposes: internal use and access,
non-commercial education, non-commercial research, and professional
services you provide to a licensee. Embedding the Software in your own
product isn't on that explicit list — it's permitted through the negative
definition instead, because shipping your own game server or backend is
not a Competing Use (your product's functionality isn't a substitute for
the framework itself). That negative test is what actually does the work:
what's off-limits is turning *the framework itself* (or something
functionally equivalent to it) into a competing product or service.

**The Change Date.** Each release of `framework` carries its own two-year
clock, starting from that release's publication date. After two years, that
specific version automatically converts to Apache License 2.0 — at which
point anyone may use it for anything, including a competing hosted service.
Because the clock is per-release, the newest code is always the protected
code, while older releases progressively become fully open. This is not
optional or revocable; the grant is irrevocable and self-executing.

**Why FSL over BSL.** Both are "eventually open" source-available licenses
with the same underlying goal. BSL 1.1 ships as a template with a blank
"Additional Use Grant" that each adopter fills in themselves — in practice
this means every BSL project restricts something slightly different
(compare MariaDB's grant to Akka's revenue-threshold grant to HashiCorp's),
so a reader has to check each project's specific wording. FSL fixes the
restriction wording (the "Competing Use" definition above) across every
adopter; the only thing an adopter customizes is which permissive license it
converts to. For a project maintained by a single person, a license with no
custom clause to draft or maintain — and one legal reading that's the same
everywhere it's used — was the deciding factor.

**Licensor.** `ZLink Systems` (see `framework/LICENSE`'s Notice section).

## 4. Why `http-client` is Apache-2.0, not FSL

The `http-client` package that exists under `framework/` in every language
is a thin wrapper over that platform's commonly used HTTP client library —
`System.Net.Http` (.NET BCL), `undici` (Node.js's own official HTTP client),
`java.net.http` (JDK standard library), and Boost.Beast (C++, already
vendored under `core/external/boost/`, Boost Software License 1.0). All four
are consumed as ordinary dependencies (via `PackageReference`/`dependencies`/
`api(...)`), not copied source — so, unlike `core`/libzmq, there's no
license-inheritance constraint on `http-client`'s own wrapper code at all.

More importantly: FSL's protection targets a threat that doesn't apply
here. Nobody stands up "HttpClient-as-a-Service" competing with an HTTP
client library — a client library is consumed inside someone else's
product, not hosted as a product in its own right. Applying FSL to it would
add adoption friction (a non-standard license a legal team has to evaluate)
for zero corresponding benefit. So `http-client` uses the plain, standard
SPDX `Apache-2.0` expression, declared per ecosystem as shown in §5, and —
since the whole point of `framework`'s FSL grant converts to Apache-2.0 after two
years anyway — `http-client` being Apache-2.0 from day one means everything
outside `core`/`bindings` converges to the same terminal license over time.

## 5. Where this is declared, per ecosystem

| Ecosystem | `framework` (FSL) | `http-client` (Apache-2.0) |
|-----------|--------------------|------------------------------|
| dotnet | `Directory.Build.props`/`.targets` set `PackageLicenseFile=LICENSE` and are configured to bundle `framework/LICENSE` into each packable `.nupkg`. **Not yet verified**: `dotnet pack` currently fails with NU5030 on this repo's local SDK (8.0.128), reproduced even in an isolated minimal project — looks like an SDK/environment bug, not a config error, but must be confirmed clean on the actual release toolchain/CI before any FSL `.nupkg` is published (see `Directory.Build.targets`'s comment) | `Zlink.HttpClient.csproj` overrides with `PackageLicenseExpression=Apache-2.0`; this path does not hit NU5030 and packs cleanly |
| java/kotlin | root `build.gradle.kts`'s `subprojects{}` Maven `pom.licenses` block | same block special-cases `zlink-http-client`/`zlink-http-client-kotlin` to Apache-2.0 |
| node | each package's `package.json` sets `"license": "SEE LICENSE IN LICENSE"`, and each package directory now ships an actual `LICENSE` file so the pointer resolves | `http-client/package.json` sets `"license": "Apache-2.0"` directly, and ships the full Apache-2.0 text as its `LICENSE` file |
| cpp | governed by `framework/LICENSE` at the tree root (no per-package manifest); every tracked FSL source file also carries a `SPDX-License-Identifier: FSL-1.1-ALv2` header | `framework/languages/cpp/http-client/LICENSE` carries the full Apache-2.0 text directly, and its source files carry `SPDX-License-Identifier: Apache-2.0` headers |
