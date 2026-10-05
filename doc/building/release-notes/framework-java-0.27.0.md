[한국어](./framework-java-0.27.0.ko.md) | [English](./framework-java-0.27.0.md)

# ZLink Java·Kotlin Framework 0.27.0 Release Notes

Framework 0.27.0 uses Java·Kotlin binding 1.14.0 and Core 1.14.0. (#1447)

## Contract changes

- Changes to capacity counts or `activationConcurrency.active` alone no longer initiate descriptor publication or a Revision increase. Location Store atomic reservations secure capacity, and the target MeshNode decides activation admission. (#1436)
- Classic fanout publishers bind their endpoints during host startup. Bind failure fails startup, and subscribers can connect before the first publish. (#1440)
- Spot `Close` follows lifecycle execution order. Ready routes carry Instance intent, allowing requests after a normal Close to activate again. Close during drain or relocation ends requests with typed failures without placing them again; Close releases authority unless it reincarnates. (#1314, #1321, #1372)
- Relocation seals retain Session-to-Actor one-way relays, with admission decided by acceptance into retention. A Join acceptance reply does not mean commit. Host shutdown seals also terminate target staging before verified cutover. (#1196, #1204, #1416)
- Actor Join to an inactive target ends with `Unavailable`; Join to a Closing target ends with `Rejected`. New admission to Closing or Draining owners follows the common error contract, and same-node Actor Join uses one local path. (#1147, #1150, #1191, #1230)
- Channel selection results and ClientServer readiness waits follow the admission deadline. Capacity refusal without a wait token ends immediately with `Unavailable`. Internal work does not hold resources while waiting for permits; application handlers may await public asynchronous APIs. (#1229, #1333, #1412)
- ClientServer application metadata is carried in the JSON header `metadata` object, with size measured using minimal escaping. (#1255, #1294)
- Status observation starts with the status at subscription time. Topology and host status Sequence increases only on publication of changed public fields. Overall RouteMesh degradation is determined by peer and Location Store state. (#1298, #1330, #1232)
- Owner lease eligibility checks use Value conditions. After Location Store Conflict, operations recheck eligibility and reconstruct the request. Intent cleanup after Store recovery uses a list reread after the owner lease TTL. Service summaries validate owner leases when counting and return query errors for lease read failures or corruption. Connection intent remains after a binding connect refusal. (#1148, #1320, #1251, #1361, #1358)
- Source cleanup failure does not prevent later cleanup steps. Relocation Store verification follows the original operation deadline, and the default Location query page size is 100. (#1313, #1302)
- STREAM is bind-only and requires binding `disconnectRid`. (#1191) STREAM session heartbeat uses the Stream Connector timeout origin. Manual dispatch queue capacity does not defer callback registration. Packet names cannot be empty or whitespace-only; unwritten frames for timed-out or cancelled requests are not sent. Transport close failure preserves the original disconnect reason and emits a `Disconnected` error event. (#1343, #1359, #1302)
- The contracts define timeout observation for application-completed asynchronous results and termination of runtime waits at host shutdown. When both an HTTP request and close fail, the request failure is returned; when only close fails after a successful request, the close failure is returned. (#1256, #1265, #1302)
- Wire failure codes, `ErrorKind`, and code-only representations of `ShuttingDown` and cause-free `InvalidOperation` follow the common error contract. Binding one-way `NOT_ADMITTED` is classified as `Rejected`. (#1367, #1369, #1302)

## Fixes and language-specific changes

- Session Actor placement reads the complete descriptor, Redis scan and history decisions were corrected, and deadlocks between the execution gate and permit owner were removed. (#1429, #1428)
- The receive owner handles ClientServer DEALER reception and closure, closing the monitor first. Windows TCP connection refusal is reported as `DISCONNECTED`. (#1392, #1426)
- Fixed Instance requests on stale Ready routes after Close, return to the same-node Entry after Actor leave, and `OnJoinedActor` lifecycle execution order. Corrected failure delivery to requests waiting on Close intent and target shutdown seals. (#1392, #1410, #1411, #1416)
- RouteMesh status no longer waits for Location Store. Fixed request backpressure, deadline progress during local permit waits, and FIFO publication order. (#1392, #1407, #1412)
- Fixed per-session relay header ownership, exact registry lookup for inbound codecs, JPMS module configuration, and GameQuest `NotConnected` failures. (#1319, #1246, #1286, #1414)

## Changes that can affect compatibility

- Ready-route Instance intent and wire error classification have changed. Wire compatibility with 0.26.0 is not guaranteed. (#1372, #1367, #1369)
- Initial status observation, Sequence, reactivation and error results after Close, and the timing of publisher bind failure may differ. (#1298, #1330, #1372, #1440)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.27.0")
    implementation("systems.zlink:zlink-http-client:0.27.0")
}
```

The release tag is [`framework-java/v0.27.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.27.0).
