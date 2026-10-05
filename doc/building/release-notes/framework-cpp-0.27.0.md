[한국어](./framework-cpp-0.27.0.ko.md) | [English](./framework-cpp-0.27.0.md)

# ZLink C++ Framework 0.27.0 Release Notes

Framework 0.27.0 uses C++ binding 1.14.0 and Core 1.14.0. (#1447)

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

- Classic fanout publisher creation and bind now occur only during host startup. (#1444)
- Location and Redis queries complete asynchronously without synchronous waits on handler workers. Redis prefix scans no longer stop because of unrelated keys. (#1393, #1395, #1401, #1439)
- Fixed GCC failures involving `co_await` inside conditional expressions, Actor metadata and STREAM relay header lifetimes across coroutine suspension, and owner lease wire field order. (#1392, #1399)
- Instance requests on stale Ready routes activate again after Close, and yielded leave callbacks remain alive until terminal completion. Fixed Close and Join during shutdown and relocation readiness handling. (#1392, #1394, #1400)
- ClientServer receive turns read pending requests within their budget. Fixed payload lifetime across relay hops and a RouteMesh perf server crash. Worker errors after readiness are no longer republished as startup failures. (#1392, #1417, #1418, #1431)
- Added public completion sources and timeout observation. Cancellation uses standard `std::system_error`, and `result_t` retains the original exception. (#1308, #1278)

## Changes that can affect compatibility

- Ready-route Instance intent and wire error classification have changed. Wire compatibility with 0.26.0 is not guaranteed. (#1372, #1367, #1369)
- Initial status observation, Sequence, reactivation and error results after Close, and the timing of publisher bind failure may differ. (#1298, #1330, #1372, #1440)
- C++ cancellation exceptions and the `result_t` exception-retention contract have changed. (#1278)

## Install

Select a platform framework archive from the [`framework-cpp/v0.27.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.27.0), or download it with `bootstrap.cmake`. The server framework requires C++ binding 1.14.0 and Core 1.14.0. Clients using only the Stream Connector do not require Core or binding.

The release tag is [`framework-cpp/v0.27.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.27.0).
