namespace ZLink.Framework.Perf;

// §10.2: what do the remote hop and relay add when the Session (Object Client) and the Actors (Object Server) run in
// different processes? Processes: CS Client x clientCount -> Session x 1 -> Actor x 1, Actor count and payload as
// §10.1. The measured operation is the connector Request.Async<PerfEchoReply> of the original STREAM request through
// the full typed echo validation; the Actor handler return value is its reply. Preparation, Store and null
// metrics are the same as §10.1.
public sealed class CsRemoteSessionActorEchoScenario(EndpointManifest manifest, Measurement measurement, int index)
    : SessionEchoOnlyScenario(manifest, measurement, index);
