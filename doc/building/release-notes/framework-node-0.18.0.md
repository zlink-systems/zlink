[English](./framework-node-0.18.0.md) | [한국어](./framework-node-0.18.0.ko.md)

# ZLink Node.js Framework 0.18.0 Release Notes

Framework 0.18.0 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The stream connector public API does not change. One observable behavior does.

- While draining, `dispatch()` returns without awaiting completion. This releases the deadlock in code that called `dispatch()` from inside a handler.

## Shared Changes

- Wrote down when a runtime descriptor change is published. The requesting side coordinates; the receiving side places no constraint on the request. (#546)
- Removed the checksum segment from the `requestContentReference` grammar of the remote-creation reservation record. A reservation is a state of the descriptor record, not a record of its own. (#559)
- Samples run one at a time. The per-language aggregate runners are gone and the common sample document owns how samples are run. (#585)
- Removed the per-language e2e scenario suites. The cross-language e2e is kept. (#541)

## Fixes

- Fixed the ZoneWorld sample failing repeatedly under WSL. Monitor event draining lived inside the receive loop, so it stopped whenever that loop parked. It now has its own loop. (#538)
- Unified 145 independent file-read sites in the contract tests, each of which carried its own newline rule and disagreed on a CRLF checkout, into a single read path. (#582)
- Moved the gate and contract assertions off the deleted aggregate runner onto the per-sample runners. (#588)

## Installation

```bash
npm install @zlink-systems/framework@0.18.0
```

The release tag is [`framework-node/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.18.0).
