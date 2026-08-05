[한국어](thread-safety.ko.md) | English

# Core Thread-Safety Implementation

## 1. Three tiers

| Tier | Meaning |
|---|---|
| Hot path | Multiple application threads may concurrently call supported socket send/request operations |
| Control path | Options, bind/connect, and handler registration are serialized per handle |
| Lifecycle | Close/destroy does not run concurrently with another mutable operation on the same handle |

## 2. Internal rules

Socket semantics protect routing state with the minimum required lock, and pipe
admission commits with message-ownership transfer. The connection's I/O thread
owns engine state. Callback mode and synchronous receive mode are single
consumers of one queue and cannot be registered together.

The public API guard manages only handle pins and close state. It does not branch
on service kind or application lifecycle. Once close starts, new operations are
rejected with the formal terminal result; active callbacks or APIs are waited
for or produce `BUSY` according to the bounded close contract.
