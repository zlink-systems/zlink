[English](./framework-java-0.18.1.md) | [한국어](./framework-java-0.18.1.ko.md)

# ZLink Java Framework 0.18.1 Release Notes

Framework 0.18.1 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The public API does not change.

## Common Changes

- The distributed zips (`zlink-tutorial-java.zip`, `zlink-samples-java.zip`) build and run without a repository checkout. Each zip root carries `README.ko.md` and `README.md` with Prerequisites, Download and install, Build, Run, Verify, and Troubleshooting sections. The `standalone-zips` CI guard runs those README command blocks verbatim in a job with no checkout. (#655, #669)
- Every Framework GitHub Release attaches eight zips (4 tutorial, 4 samples). Core and binding releases carry the same assets. (#639)
- The Java and Kotlin tutorials gain the same Instance Spot queue (`MatchQueue`) as .NET. (#666)
- Sample runners no longer depend on Python. The ZoneWorld ZW-B8 proxy is `SessionRouteBlockProxy` and port reservation is `ReservePorts`, both Java programs. (#673)
- The guide gains a read-along chapter (50–56) for each of the seven samples, and chapters 01 and 03 read their code from tutorial snippets. (#640, #641)

## Fixes

- A waiter now ends as `Disconnected` the moment the connection it observed ends. It used to end when the next connection was established, so with no reconnect after a drop it hung until its own timeout (spec 32 §10.1.1). (#667)
- The Windows `bin/<app>.bat` produced by `installDist` listed every jar on one line and exceeded the 8191-character limit of cmd.exe ("The input line is too long"; 69 jars in the Kotlin tutorial). The classpath is now `lib/*`. (#655)
- Sample runners assumed ripgrep (`rg`) and died with `rg: command not found` on a stock Ubuntu; they now use POSIX `grep`. Redis container id extraction, which always failed under the default Ubuntu awk (mawk), is fixed too. (#655)
- Nine sample runners tried to rebuild the framework jars even inside the distributed zip. Outside a checkout they use only the Maven Central packages. (#655)
- Bingo, DeliveryDispatch, ShoppingMall, GameQuest, and ZoneWorld samples (Java and Kotlin) now match the canonical contracts. Bingo re-checks membership after `Yield` and only logs on disconnect. DeliveryDispatch sends HTTP→dispatch channel messages one-way and observes sweeper failures. ShoppingMall closes the terminal Instance Spot and blocks duplicate terminal continuations. GameQuest uses the kill count from the GameplayStateStore snapshot. ZoneWorld configures the incoming border loop in configure and drops the payload toZone filter. (#658, #662, #663, #664, #665)

## Installation

```kotlin
implementation("systems.zlink:zlink-framework-core:0.18.1")
```

The release tag is [`framework-java/v0.18.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.18.1).
