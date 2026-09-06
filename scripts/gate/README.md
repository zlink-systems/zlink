# Framework gates

Serialized, machine-local gates used to close a Core/framework change. Logs go to `zlink-work/gates/<tag>/`.

| Script | What it does |
|---|---|
| `rebuild-dev.sh <tag>` | `core/build-dev` rebuild → local Core prefix (`~/.cache/zlink/core`) → cpp/dotnet/java/node local packages → node/java reinstall. Run only when no gate is running. |
| `framework-gate.sh <tag>` | 7 samples × cpp/java/node/dotnet (+ dotnet ZoneWorld ×2), node `npm test` (incl. M6A), java core/contract tests, dotnet sample-regression + unit tests. |
| `cross-language-e2e.sh <tag>` | cpp all-stage, node smoke, java-cross cross-language E2E. |

Rules: one gate at a time (samples use fixed ports, tests assert timing); `require_quiet` refuses to start above load average 10 (`ZLINK_GATE_MAX_LOAD`). Run long gates detached: `setsid nohup scripts/gate/framework-gate.sh g15 > zlink-work/gates/g15.out 2>&1 &` and watch `zlink-work/gates/g15/results.txt`.
