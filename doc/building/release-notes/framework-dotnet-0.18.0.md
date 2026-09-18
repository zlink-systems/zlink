[English](./framework-dotnet-0.18.0.md) | [한국어](./framework-dotnet-0.18.0.ko.md)

# ZLink .NET Framework 0.18.0 Release Notes

Framework 0.18.0 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The stream connector public surface changes.

- The three connection events change from C# `event`s to methods that return an `IDisposable`. Replace `+=` registrations with method calls and keep the returned value until you dispose it.
- Errors are unified into a closed set of 13 codes. A send-limit violation, rejected before anything reaches the transport, is `ValidationFailed`.
- Metrics were removed from the client connector.

## Shared Changes

- Wrote down when a runtime descriptor change is published. The requesting side coordinates; the receiving side places no constraint on the request. (#546)
- Removed the checksum segment from the `requestContentReference` grammar of the remote-creation reservation record. A reservation is a state of the descriptor record, not a record of its own. (#559)
- Samples run one at a time. The per-language aggregate runners are gone and the common sample document owns how samples are run. (#585)
- Removed the per-language e2e scenario suites. The cross-language e2e is kept. (#541)

## Fixes

- Fixed a ZoneNode stopped by a crash failing to reclaim its previous zone objects when restarted under the same NodeId, so it never reached ready. (#555)
- Fixed a cancellation escaping the zone Spot rejoin path after a node restart and aborting the process. (#542)
- Fixed runtime descriptor mutations not waking the loop, which deferred publication. The four setters now pass through a single choke point. (#516)
- Fixed sample teardown: a role that was force-killed no longer passes silently through the bash runner. (#575)
- Fixed sample regression tests pinning newlines as literal characters, which failed on a CRLF checkout. (#578)

## Installation

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.18.0
```

The release tag is [`framework-dotnet/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.18.0).
