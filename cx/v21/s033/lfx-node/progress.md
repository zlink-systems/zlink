# Node ladder measurement progress

- Method: 10 s warmup, 5 s active; source CPUs 0–5, target CPUs 8–13; C2/C3 off; SCHED_OTHER; 6 cells, one counted run/step. Every change gets a discarded warm run; all raw files retained.
- Branch/HEAD at setup: `exp/4350-node-fixes` / `50f84349c69503082774760b05834e513bdae785`; merge-base pending build note.
- Build and SHA-256 fingerprints pending.
- [Call shape and per-message costs](call-shape.md).

## Build fingerprints
- rc: http-client local package 0; npm ci 0; use:bindings-source 0; npm run build 0.
- HEAD=50f84349c69503082774760b05834e513bdae785 dirty=clean branch=exp/4350-node-fixes merge_base=6350275facc547759c9e143cc2515f3f528c776e
- freshness: Node production TS→JS outputs checked=424, missing=0, stale=0; 1 contract test TS has no dist output by design.
- framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05 mtime=1985-10-26 17:15:00.000000000 +0900
- framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f mtime=2026-11-14 08:30:35.174408955 +0900
- framework/languages/node/packages/http-client/dist/index.js sha256=64c839a760746f100af6668ed69f720f415cdb8c276769e73a8c5bfde843da89 mtime=2026-11-14 08:30:43.737059436 +0900
- framework/languages/node/packages/stream-wire/dist/index.js sha256=64002b54142361b539753a79bef34e49eece83067c1eb73e077df7acd2ebe619 mtime=2026-11-14 08:30:19.613044157 +0900
