namespace ZLink.Framework.Perf;

// §10.1: how much do connector, session and Actor dispatch cost together when the session and the Actors it binds
// share one Object Server node? Processes: CS Client x clientCount -> SessionActorLocal x 1.
// One operation = the connector Request.Async<PerfEchoReply> of the original STREAM request through the full typed
// echo validation. The session relays it to the Actor bound to that connector; that Actor's handler return value is
// the reply. The setup probe names the connector ID, so the session creates and binds its Actor during setup.
// 1024 bytes JSON, request/ordinary. Store: run Redis (Object Server). Actor source admission, Spot, worker and
// fanout metrics do not apply; internal Spot metrics are not publicly observable.
public sealed class CsLocalSessionActorEchoScenario(EndpointManifest manifest, Measurement measurement, int index)
    : SessionEchoOnlyScenario(manifest, measurement, index);
