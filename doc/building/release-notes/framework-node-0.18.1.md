[English](./framework-node-0.18.1.md) | [한국어](./framework-node-0.18.1.ko.md)

# ZLink Node.js Framework 0.18.1 Release Notes

Framework 0.18.1 uses binding 1.2.1 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

- stream-connector (TypeScript): the timeout error code of `waitFor` and `waitForSequence` changes from `RequestTimeout` to `ValidationFailed` (spec 32 §10.1.1). `RequestTimeout` is used only for a request's reply wait. `expectNone` is unaffected. Callers that inspected the timeout code switch to `ValidationFailed`. (#667)

## Common Changes

- The distributed zips (`zlink-tutorial-node.zip`, `zlink-samples-node.zip`) build and run without a repository checkout. Each zip root carries `README.ko.md` and `README.md` with Prerequisites, Download and install, Build, Run, Verify, and Troubleshooting sections. The `standalone-zips` CI guard runs those README command blocks verbatim in a job with no checkout. (#655, #669)
- Every Framework GitHub Release attaches eight zips (4 tutorial, 4 samples). Core and binding releases carry the same assets. (#639)
- The Node tutorial gains the same Instance Spot queue (`MatchQueue`) as .NET. (#666)
- The guide gains a read-along chapter (50–56) for each of the seven samples, and chapters 01 and 03 read their code from tutorial snippets. (#640, #641)

## Fixes

- A waiter now ends as `Disconnected` the moment the connection it observed ends. It used to end when the next connection was established, so with no reconnect after a drop it hung until its own timeout (spec 32 §10.1.1). (#667)
- The published `@zlink-systems/stream-connector` package was missing `dist/package.json`, which broke ESM resolution. (#655)
- In the distributed samples zip, `prepare-sample-dependencies.mjs` mistook the unzipped directory for a repository checkout and all seven samples died in `prebuild` with `ENOENT`. It now checks the repository workspace root `package.json` by name. (#655)
- Bingo, DeliveryDispatch, GameQuest, and ZoneWorld samples now match the canonical contracts. Bingo rooms register with `PreserveStateWith` and application-signaled readiness and defer relocation-ready at a completed-round boundary. DeliveryDispatch logs and ignores a courier decision for a missing offer. GameQuest sync reads the store snapshot and `PlayerQuestSpot` binds the player ID in initialize. ZoneWorld deduplicates join completions by OperationId and preserves it in the relocation payload. (#658, #662, #664, #665)
- Binding 1.2.1 ships a win32-x64 prebuild in the npm package, so Windows installs without a source build. (#656)

## Installation

```bash
npm install @zlink-systems/framework@0.18.1
```

The release tag is [`framework-node/v0.18.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.18.1).
