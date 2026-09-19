[English](./framework-dotnet-0.18.1.md) | [한국어](./framework-dotnet-0.18.1.ko.md)

# ZLink .NET Framework 0.18.1 Release Notes

Framework 0.18.1 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The public API does not change.

## Common Changes

- The distributed zips (`zlink-tutorial-dotnet.zip`, `zlink-samples-dotnet.zip`) build and run without a repository checkout. Each zip root carries `README.ko.md` and `README.md` with Prerequisites, Download and install, Build, Run, Verify, and Troubleshooting sections. The `standalone-zips` CI guard runs those README command blocks verbatim in a job with no checkout. (#655, #669)
- Every Framework GitHub Release attaches eight zips (4 tutorial, 4 samples). Core and binding releases carry the same assets. (#639)
- Sample runners no longer depend on Python. Role configuration JSON is written by bash heredocs, and the ZoneWorld ZW-B8 proxy is a dependency-free `net8.0` console project. (#673)
- The guide gains a read-along chapter (50–56) for each of the seven samples, and chapters 01 and 03 read their code from tutorial snippets. (#640, #641)

## Fixes

- A waiter now ends as `Disconnected` the moment the connection it observed ends. It used to end when the next connection was established, so with no reconnect after a drop it hung until its own timeout. `Close` releases waiters the same way (spec 32 §10.1.1). (#667)
- In the distributed samples zip, `sample_runner.ps1` unconditionally dot-sourced the repository-only `local_nuget.ps1`, so all seven samples died on Windows. Outside a checkout the samples reference only the `Zlink.Framework` package from nuget.org. (#655)
- Bingo, TicTacToe, and SupportChat samples now match the canonical contracts. The Bingo Session callback neither iterates bound Actors nor removes bindings itself. TicTacToe Api and Play keep a fixed RID and the contrary known-deviation comment is gone. Every SupportChat Actor factory selects `DisableRelocation` and no Relocation Store is registered. (#658, #659, #660)
- The tutorial README builds and runs the Release configuration throughout; it used to build Debug and run the Release path. (#655)

## Installation

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.18.1
```

The release tag is [`framework-dotnet/v0.18.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.18.1).
