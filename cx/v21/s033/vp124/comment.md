## Core 1.24.0 tag condition: package verification (kairos, cloudv)

- Source: PR #4166 head `8bc547de3c`, Core built Release (`-DBUILD_SHARED=ON`, tests/benchmarks off), prefix made with `scripts/gate/materialize-local-core-prefix.sh` (provenance included).
- `scripts/local-package/core/verify-package.sh --prefix …/1.24.0/linux-x64`: **PASS** — `Core package verified: version=1.24.0 abi=0`.
- Runtime `libzlink.so.1.24.0` SHA-256: `83a0dff83544dd2f674553bee41ba09898e06a40566532e4374a527a5bc62320` (local build; the release asset from `build.yml` will differ).
- `scripts/local-package/build-wsl.sh --verify-versions`: rc=1 as expected. It lists exactly the 14 Framework pin files plus `vcpkg/ports/zlink-framework/portfile.cmake`, which #4166 deliberately left at 1.23.0 for the post-publish Framework pin PR. No other difference.
- Other tag conditions from the gate run: hotpath_gate PASS, wake invariants until-fail:10 PASS (`core124/logs/`); release comparison exception approved (this issue).

Logs (cloudv): `/root/jobs/fx/cx/v21/s033/vp124/{verify-package.log,verify-versions.log,build.log,materialize.log,runtime.sha256}`
