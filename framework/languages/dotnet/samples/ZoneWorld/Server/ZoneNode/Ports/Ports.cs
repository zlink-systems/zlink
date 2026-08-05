namespace ZoneWorld.Server.ZoneNode.Ports;

/// <summary>
/// The desired maintenance state, owned by the operator and outlived by any node
/// restart (§8.4). ZLink does not provide a store for this: it is application state,
/// so the node reads it from the same Redis the location store uses.
/// </summary>
public interface IMaintenanceStorePort
{
    ValueTask<IReadOnlyDictionary<string, bool>> ReadAllAsync(CancellationToken cancellationToken);
}

/// <summary>
/// What this node tells the Ops console. Provider-neutral timer failure events are received
/// by the owning node and reported explicitly (§8.1).
/// </summary>
public interface IOpsReportPort
{
    ValueTask ReportSpotEventAsync(
        string kind,
        string detail,
        CancellationToken cancellationToken);

    ValueTask ReportNodeStatusAsync(
        IReadOnlyList<string> zones,
        int playerCount,
        bool maintenance,
        CancellationToken cancellationToken);
}
