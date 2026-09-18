[English](./framework-java-0.18.0.md) | [한국어](./framework-java-0.18.0.ko.md)

# ZLink Java Framework 0.18.0 Release Notes

Framework 0.18.0 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The stream connector public surface changes.

- A `ZLinkStreamException` carrying an error code is thrown instead of standard exceptions. Standard exceptions had nowhere to carry the code, so callers could not tell `ValidationFailed` from `ConfigurationError`.
- `ZLinkStreamConnectorOptions` is a record with 21 components. **Positional construction breaks every time a component is added** - derive from `createDefault` instead.
- The Kotlin wrapper (`ZLinkKotlinStreamConnector`) gains a `closeReason()` that reads the close reason, and `expectNone()`/`waitForSequence()` can now derive the name from the type alone, without naming it. (#600)

## Shared Changes

- Wrote down when a runtime descriptor change is published. The requesting side coordinates; the receiving side places no constraint on the request. (#546)
- Removed the checksum segment from the `requestContentReference` grammar of the remote-creation reservation record. A reservation is a state of the descriptor record, not a record of its own. (#559)
- Samples run one at a time. The per-language aggregate runners are gone and the common sample document owns how samples are run. (#585)
- Removed the per-language e2e scenario suites. The cross-language e2e is kept. (#541)

## Fixes

- Fixed the ActorClient message-follow test failing intermittently in a full run with `one-way route is not connected`. The send path did not allow `VALIDATING_PREVIOUSLY_READY`, which left a race. (#533)
- Fixed the inline-v1 reference in the reservation record lacking a CRC32C segment, which rejected every reservation written by another language. (#559)
- Fixed receive-queue draining (`ZLinkStreamDispatchQueue.drainAsync`) recursing into itself per item, which ended in a `StackOverflowError` once the queue grew long. It now drains with a loop. (#604)
- Fixed a ZoneWorld node restarted after a crash coming up in a cold-start configuration that claims zones, so it never reclaimed any and never reached topology ready. Restarts now use a replacement configuration that starts with zero zones. (#621)
- Fixed Windows PowerShell 5.1's `Start-Process` returning a process object with no OS handle, so `ExitCode` came back `$null` and a TicTacToe client that finished its scenario was judged a failure. Every Java/Kotlin sample runner now starts child processes through the shared `Start-ZlinkSampleProcess` helper. (#628)
- Fixed Windows ZoneWorld's `ZW-B8` stalling because it could not find its fault proxy's Python, either off PATH or resolving to a Store alias stub that will not run. `Get-ZlinkSamplePythonCommand` now checks PATH, the `py` launcher, and the standard install locations in turn, accepting only a candidate that actually reports a working Python 3. (#633)

## Installation

```kotlin
implementation("systems.zlink:zlink-framework-core:0.18.0")
```

The release tag is [`framework-java/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.18.0).
