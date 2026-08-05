namespace ZoneWorld.Shared.Contracts;

// The wire contract from scenario §7. The browser client mirrors these records in
// TypeScript; the field names and their meaning are the contract, so a rename here
// is a wire break for every language server and the client.

public sealed record PlayerView(string PlayerId, int X, int Y, string ZoneId, bool IsBot);

public sealed record NodeView(
    string NodeId,
    bool Registered,
    bool Connected,
    bool Maintenance,
    IReadOnlyList<string> Zones,
    int PlayerCount);

// ---------------------------------------------------------------------------
// §7.1 game — browser <-> Gateway (STREAM)
// ---------------------------------------------------------------------------

public sealed record JoinWorldReq(string PlayerId);

public sealed record JoinWorldRes(
    string PlayerId,
    string ZoneId,
    int X,
    int Y,
    string? Error = null);

public sealed record MoveMsg(int X, int Y);

public sealed record ZoneStateNotify(string ZoneId, long Tick, IReadOnlyList<PlayerView> Players);

public sealed record ZoneChangedNotify(string PlayerId, string ZoneId);

public sealed record WorldAnnounceNotify(string AnnouncementId, string Text);

public sealed record MoveRejectedNotify(string Reason, int X, int Y);

// ---------------------------------------------------------------------------
// §7.2 ops console — browser <-> Ops (STREAM)
// ---------------------------------------------------------------------------

public sealed record WatchNodesReq;

public sealed record WatchNodesRes(IReadOnlyList<NodeView> Nodes);

public sealed record NodeStatusNotify(
    string NodeId,
    bool Registered,
    bool Connected,
    bool Maintenance,
    IReadOnlyList<string> Zones,
    int PlayerCount);

public sealed record NodeAlertNotify(string NodeId, string Kind, string Detail, string OccurredAt);

public sealed record AnnounceWorldReq(string Text);

public sealed record AnnounceWorldRes(string AnnouncementId);

public sealed record SetMaintenanceReq(string NodeId, bool Enabled);

public sealed record SetMaintenanceRes(
    string NodeId,
    bool Enabled,
    IReadOnlyList<string> Zones,
    string? Error = null);

public sealed record NodeDiagnosticsReq(string NodeId);

public sealed record NodeDiagnosticsRes(
    string NodeId,
    IReadOnlyList<string> Zones,
    int PlayerCount,
    bool Maintenance,
    string? Error = null);

/// <summary>
/// Ops-only self-check request. It selects adjacent Zone Spots whose current owners
/// differ without turning either owner RID into an application routing address.
/// </summary>
public sealed record RelocationPairReq;

public sealed record RelocationPairRes(
    string SourceZoneId,
    string TargetZoneId,
    string SourceOwnerNodeRid,
    string TargetOwnerNodeRid,
    string? Error = null);

/// <summary>
/// Ops-only self-check request used to verify that relocation preserves the Actor
/// object generation while changing its current owner.
/// </summary>
public sealed record ActorLocationProbeReq(string ActorId);

public sealed record ActorLocationProbeRes(
    string ActorId,
    ulong ObjectGeneration,
    string OwnerNodeRid,
    string? Error = null);

// ---------------------------------------------------------------------------
// §7.3 server internal
// ---------------------------------------------------------------------------

/// <summary>Ops -> every ZoneNode (fanout `zoneworld.broadcast`, topic `world.announce`).</summary>
public sealed record WorldAnnounceEvent(string AnnouncementId, string Text);

/// <summary>Ops -> every ZoneNode (fanout, topic `world.maintenance`). Feeds the per-node cache (§2.3).</summary>
public sealed record NodeMaintenanceChangedEvent(string NodeId, bool Enabled);

/// <summary>Fanout subscriber -> the zone spots of its own node, through the spot bridge (§8.2).</summary>
public sealed record DeliverAnnounceMsg(string AnnouncementId, string Text);

/// <summary>
/// Zone spot -> bot actor. Drives one patrol step (§2.7). This is a request so the periodic
/// producer cannot build an unbounded movement backlog while the actor is relocating.
/// </summary>
public sealed record BotTickMsg();

/// <summary>
/// Ensure handler -> the freshly created player actor, while it still sits in the entry
/// spot. The actor answers by joining its zone spot, which is the only way to enter a
/// zone (§2.6), and reports back where it landed. The caller has to wait for that join
/// before it can bind a session, which is why this is a request and not a send.
/// </summary>
public sealed record EnterWorldReq(int X, int Y, bool IsBot, int DirX = 0, int DirY = 0);

public sealed record EnterWorldRes(string ZoneId, int X, int Y, string? Error = null);

/// <summary>ZoneNode -> Ops (channel `zoneworld.report`). Sent when the event occurs (§8.1).</summary>
public sealed record ReportSpotEventMsg(string NodeId, string Kind, string Detail, string OccurredAt);

/// <summary>ZoneNode -> Ops (channel `zoneworld.report`). Sent every second (§8.1).</summary>
public sealed record ReportNodeStatusMsg(
    string NodeId,
    IReadOnlyList<string> Zones,
    int PlayerCount,
    bool Maintenance);

/// <summary>
/// Zone spot -> adjacent zone spot (spot pub/sub, topic `zone.border.&lt;from&gt;.&lt;to&gt;`).
/// Loss is allowed; the receiver replaces per FromZoneId and expires stale snapshots (§2.4).
/// </summary>
public sealed record ZoneBorderEvent(
    string FromZoneId,
    string ToZoneId,
    long Tick,
    IReadOnlyList<PlayerView> Players);

/// <summary>
/// Player actor -> zone spot, as the JoinSpot admission payload. Joining is what
/// moves the actor between zones, and across nodes it is what triggers the actor
/// relocation, so the zone change rides the join rather than a plain send (§2.6).
/// The payload carries the global ActorId only. The framework resolves the current
/// owner, so application messages never carry a cached ActorRef (§8.3). The logical
/// source NodeId is included only for the target admission rule: maintenance allows a
/// player to move between zones already hosted by the same node, but rejects a new or
/// cross-node arrival.
/// </summary>
public sealed record EnterZoneMsg(
    string PlayerId,
    int X,
    int Y,
    bool IsBot,
    bool InitialEntry,
    string? FromNodeId);

public sealed record EnterZoneRes(string ZoneId, string? Error = null);

/// <summary>
/// Player actor -> current Zone Spot. Same-zone movement updates the Spot's rendering
/// projection through this message; the actor remains the coordinate authority (§2.1, §7.1).
/// </summary>
public sealed record UpdatePositionMsg(
    string PlayerId,
    int X,
    int Y,
    bool IsBot);

/// <summary>
/// Zone spot -> player actor. The actor forwards the snapshot through its current
/// bound session, so relocation and Message Follow remain on the normal Actor route.
/// </summary>
public sealed record DeliverZoneStateMsg(
    string ZoneId,
    long Tick,
    IReadOnlyList<PlayerView> Players);

public sealed record DeliverZoneChangedMsg(string PlayerId, string ZoneId);

/// <summary>
/// Zone spot -> player actor. Bots receive no instance of this message because they
/// do not have a bound client session.
/// </summary>
public sealed record DeliverWorldAnnounceMsg(string AnnouncementId, string Text);

/// <summary>
/// Runner-only Actor request used after relocation to verify that Message Follow
/// preserves the original payload and reply route.
/// </summary>
public sealed record MessageFollowProbeReq(string ActorId, string ProbeId, byte[] Payload);

public sealed record MessageFollowProbeRes(string ProbeId, byte[] Payload);
