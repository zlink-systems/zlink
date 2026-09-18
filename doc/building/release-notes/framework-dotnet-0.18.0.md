[English](./framework-dotnet-0.18.0.md) | [한국어](./framework-dotnet-0.18.0.ko.md)

# ZLink .NET Framework 0.18.0 Release Notes

Framework 0.18.0 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The stream connector public surface changes.

- The three connection events change from C# `event`s to methods that return an `IDisposable`. Replace `+=` registrations with method calls and keep the returned value until you dispose it.
- Errors are unified into a closed set of 13 codes. A send-limit violation, rejected before anything reaches the transport, is `ValidationFailed`.
- Metrics were removed from the client connector.
- `close()` no longer waits for the disconnect handler to finish; it runs the handler and returns right away. This releases the deadlock in code that called `close()` from inside that handler. (#590)

## Shared Changes

- Wrote down when a runtime descriptor change is published. The requesting side coordinates; the receiving side places no constraint on the request. (#546)
- Removed the checksum segment from the `requestContentReference` grammar of the remote-creation reservation record. A reservation is a state of the descriptor record, not a record of its own. (#559)
- Samples run one at a time. The per-language aggregate runners are gone and the common sample document owns how samples are run. (#585)
- Removed the per-language e2e scenario suites. The cross-language e2e is kept. (#541)

## Fixes

- On Windows, the ZoneWorld `ZW-B8` fault proxy could not find a Python install that is off PATH, or picked the Store alias stub that runs nothing. The runner now checks PATH, the `py` launcher and the standard install roots in turn, and accepts only an interpreter that actually reports Python 3. (#642)
- Fixed a ZoneNode stopped by a crash failing to reclaim its previous zone objects when restarted under the same NodeId, so it never reached ready. (#555)
- Fixed a cancellation escaping the zone Spot rejoin path after a node restart and aborting the process. (#542)
- Fixed runtime descriptor mutations not waking the loop, which deferred publication. The four setters now pass through a single choke point. (#516)
- Fixed sample teardown: a role that was force-killed no longer passes silently through the bash runner. (#575)
- Fixed sample regression tests pinning newlines as literal characters, which failed on a CRLF checkout. (#578)
- Fixed the `.NET` ShoppingMall sample's PowerShell runner querying ownership of the order's Instance Spot instead of the planned-relocation Spot that actually moves, so it could not properly confirm relocation. Also removed the runner advancing the order itself, so it stays observation-only. (#605)
- Fixed `ZW-G3`'s new-object verdict depending on zone-nw's survival, which made it fail depending on which node the crash lane picked. It now uses a dedicated fresh-object probe that does not depend on any zone. (#567)
- Rebuilt or removed three sample regression tests that failed by reading the deleted `run_samples` aggregate runner, basing each on the evidence the current runners actually leave. (#568)

## Installation

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.18.0
```

The release tag is [`framework-dotnet/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.18.0).
