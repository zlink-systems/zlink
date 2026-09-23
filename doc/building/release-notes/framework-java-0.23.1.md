[English](./framework-java-0.23.1.md) | [한국어](./framework-java-0.23.1.ko.md)

# ZLink Java·Kotlin Framework 0.23.1 Release Notes

Framework 0.23.1 uses binding 1.4.0 and Core 1.4.0. Framework language releases are versioned independently.

## Contract changes

The public API does not change.

## Fixes

- When a manually registered peer endpoint had not connected yet and its Location Store descriptor appeared first, the replacement with the descriptor values was refused on every attempt and the two nodes never connected. A connection intent that never connected is now closed once Core's `disconnect` succeeds. This was the intermittent repeated `not_found` when a server and client start at the same time. (#1034)
- The Kotlin tutorial README's run and verify blocks called the Java port (5280); they now call the Kotlin port (5380). (#1035)
- The ZoneWorld ZW-B8 fault proxy moved to the shared `samples/Support/`, so the Java and Kotlin samples use one file. The Kotlin sample distribution could not find the proxy before. (#1035)
- The Kotlin ZoneWorld reporter lifecycle test is now written in Kotlin, which removes the compile error from calling a suspend function from Java. (#1035)
