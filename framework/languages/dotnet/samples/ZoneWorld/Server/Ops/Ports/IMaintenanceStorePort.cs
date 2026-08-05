namespace ZoneWorld.Server.Ops.Ports;

/// <summary>
/// The operator's desired maintenance state (§8.4). Ops writes it; a ZoneNode reads it at
/// startup. It is application state — ZLink offers no store for it — and it has to outlive
/// a node restart, which a fanout cannot promise.
/// </summary>
public interface IMaintenanceStorePort
{
    ValueTask WriteAsync(string nodeId, bool enabled, CancellationToken cancellationToken);
}
