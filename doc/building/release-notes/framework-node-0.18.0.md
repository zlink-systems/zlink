[English](./framework-node-0.18.0.md) | [한국어](./framework-node-0.18.0.ko.md)

# ZLink Node.js Framework 0.18.0 Release Notes

Framework 0.18.0 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The stream connector public API does not change. Three observable behaviors do.

- While draining, `dispatch()` returns without awaiting completion. This releases the deadlock in code that called `dispatch()` from inside a handler.
- A send-payload-over-limit violation now returns `ValidationFailed` instead of `FrameTooLarge`, the code the common spec requires. C++ fixed the same defect in #599. (#618)
- `close()` no longer waits for the disconnect handler to finish; it runs the handler and returns right away. This releases the deadlock in code that called `close()` from inside that handler. (#590)

## Shared Changes

- Wrote down when a runtime descriptor change is published. The requesting side coordinates; the receiving side places no constraint on the request. (#546)
- Removed the checksum segment from the `requestContentReference` grammar of the remote-creation reservation record. A reservation is a state of the descriptor record, not a record of its own. (#559)
- Samples run one at a time. The per-language aggregate runners are gone and the common sample document owns how samples are run. (#585)
- Removed the per-language e2e scenario suites. The cross-language e2e is kept. (#541)

## Fixes

- On reconnect, the previous connection's unconsumed messages stayed queued, so `receivedCount` read 0 while `waitFor` handed back an old message. The counts and the queue are now cleared together, and a wait that observed the previous connection ends with `Disconnected` (spec §10). (#583)
- Fixed the ZoneWorld sample failing repeatedly under WSL. Monitor event draining lived inside the receive loop, so it stopped whenever that loop parked. It now has its own loop. (#538)
- Unified 145 independent file-read sites in the contract tests, each of which carried its own newline rule and disagreed on a CRLF checkout, into a single read path. (#582)
- Moved the gate and contract assertions off the deleted aggregate runner onto the per-sample runners. (#588)
- Fixed the runtime gate assuming `node --test`'s default reporter was TAP on Node 23 and later, which counted every passing test as a failure. The reporter is now pinned with `--test-reporter=tap`, and the supported Node versions are recorded in `engines`. (#520)
- Fixed the unity-webgl contract test on Windows passing a raw absolute path to a dynamic `import()`, which failed with `ERR_UNSUPPORTED_ESM_URL_SCHEME`. It now uses `pathToFileURL(...).href` like every other such test. (#522)

## Installation

```bash
npm install @zlink-systems/framework@0.18.0
```

The release tag is [`framework-node/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.18.0).
