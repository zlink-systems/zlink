[English](./framework-cpp-0.30.2.md) | [한국어](./framework-cpp-0.30.2.ko.md)

# ZLink C++ Framework 0.30.2 Release Notes

Framework 0.30.2 uses C++ binding 1.20.0 and Core 1.20.0.

## Compatibility changes

- The `is_ready` value of the host's own RouteMesh is `true` only while the host is `serving`. Version 0.30.1 read it from the Location Store row, so it could be `true` while hosted services were still starting. Hosted services must finish starting before the host becomes `serving`, so a `start()` that waits for its own mesh to become ready never lets startup finish. Run the readiness check in separate work that `start()` does not wait for. (#2207)

## Changes

- Request admission is distinct from reply completion, so callers can continue after acceptance without waiting for a reply. Join and deferred work completion is also delivered in Actor FIFO order. (#2069, #2040, #2119)
- Channel receive phases—received, admitted, and dispatched—are recorded in message flow. (#2412)
- Reduced per-message topology and owner-lease reads and completion work. Headers are decoded once at ingress, and payloads are transferred without intermediate copies. (#1829, #2091)
- Records runtime metrics (STREAM connections, Actor population, Location lease, relocation outcome) at the observation points the spec defines. (#2189)

## Defect Fixes

- Fixed mismatched authority, sealed inventory, and completion ownership during relocation and Join that could leave Actors duplicated or stall shutdown and handoff. (#1923, #2048, #2039, #2086, #2121, #2124, #2386, #2432)
- Fixed duplicate ownership caused by reading the previous authority during aggregate commit. (#2423)
- Fixed late Spot request terminals reaching a released resolver and requests failing before the new owner route was ready after relocation. (#2416, #2414)
- Fixed stream connection retirement racing with the lifetime of dispatch callbacks. (#2302)
- Fixed platform-specific defects and test environment issues affecting Windows and macOS builds and Boost-based execution. (#2145, #2035, #2322)
- Fixed a Bingo sample issue where an observer joining late missed an already-awarded reward. The sample now looks up the current state and confirmed rewards after the observer joins. (#2427)
- Fixed replies to requests that reached the new Entry Spot after relocation not reaching the client. Source retirement, sealed inventory and authority encoding are each decided in one place, and synchronous waits inside execution turns are removed. (#2109, #1788)
- Core 1.20.0 no longer stalls the temporary transport owner's executor when a completion poller is registered during the handshake. (#2504)
- Actor replies are sent as a single STREAM frame, and error payloads use the spec's `{code, message}` form. All four languages verify against the same shared vectors. (#2507)
- Fixed Session bind being rejected with a stale authority when an Actor returned to the same node after relocation, requests returning through Message Follow not being processed, and relocation terminal replies going to the coordinator instead of the original requester. (#2507)
- Updated the GameQuest sample to send the next lookup after it observes Close. (#2507)
- Fixed `get_or_create` on a User Spot that moved in by relocation creating a new Spot instead of returning the existing one. (#2607)
- A Channel send owns the call arguments it uses after suspension, which fixes sending wrong values when the caller's arguments end first. (#2507)
- A STREAM host reads port `*` as a request for a free port. The Windows resolver rejected service `*` and the host terminated abnormally. (#2618)
- Runtime status queries and observation use the same published status, and the first observed item is the current status at subscription. (#2629)

## Known issues

- The relocation and authority fix (#2128) increased the cost of stateful Spot requests. In a same-node measurement one request drain is about 5 times slower than the code before #2128 (this release removes coroutine frames and allocations, which makes it 18% faster than right after the fix). The rest comes from admission passing through other threads even when the gate is idle, and is fixed in 0.31.0. (#1672)

## Install

Select a platform Framework archive from the [`framework-cpp/v0.30.2` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.2), or download it with `bootstrap.cmake`. A server Framework requires C++ binding 1.20.0 and Core 1.20.0. A client that uses only the Stream Connector does not require Core or the binding.

The release tag is [`framework-cpp/v0.30.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.2).
