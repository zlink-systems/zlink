using System.Collections.Concurrent;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Streams;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Ops.Infrastructure.ZLink.Sessions;

/// <summary>
/// The consoles currently watching. Node state changes arrive from runtime events and node
/// reports, which are not tied to any one session, so the push targets are kept here.
/// </summary>
public sealed class OpsConsoleRegistry(ILogger<OpsConsoleRegistry>? logger = null)
{
    private const int RecentAlertCount = 20;

    private readonly ConcurrentDictionary<string, IZLinkSessionContext> _consoles = new(StringComparer.Ordinal);
    private readonly Dictionary<string, NodeStatusNotify> _latestNodes = new(StringComparer.Ordinal);
    private readonly Queue<NodeAlertNotify> _recentAlerts = new();
    private readonly object _nodeGate = new();

    /// <summary>
    /// Registers a console without sending application data from the stream lifecycle callback.
    /// The framework guarantees the callback ordering, but a route may still be unavailable for
    /// an application push at that point. State replay therefore belongs to the request that
    /// explicitly starts watching, after its reply has established the stream path.
    /// </summary>
    public void Add(IZLinkSessionContext context) => _consoles[context.SessionId] = context;

    /// <summary>
    /// Replays the latest node state to a console that has completed the watch request. The
    /// cache is updated by node broadcasts independently of any particular stream session.
    /// </summary>
    public async ValueTask ReplayNodesAsync(
        IZLinkSessionContext context,
        CancellationToken cancellationToken)
    {
        NodeStatusNotify[] snapshot;
        lock (_nodeGate) snapshot = _latestNodes.Values.ToArray();

        foreach (var node in snapshot)
            await SendAsync(context, node, cancellationToken);
    }

    /// <summary>
    /// Hands a console the alerts that arrived before it started watching. A node fault does
    /// not wait for an operator to be at the screen, and an alert nobody ever sees is an alert
    /// that, to the operator, did not happen.
    /// </summary>
    public async ValueTask ReplayAlertsAsync(
        IZLinkSessionContext context,
        CancellationToken cancellationToken)
    {
        NodeAlertNotify[] backlog;
        lock (_recentAlerts) backlog = _recentAlerts.ToArray();

        foreach (var alert in backlog)
            await SendAsync(context, alert, cancellationToken);
    }

    public void RecordAlert(NodeAlertNotify alert)
    {
        lock (_recentAlerts)
        {
            _recentAlerts.Enqueue(alert);
            while (_recentAlerts.Count > RecentAlertCount) _recentAlerts.Dequeue();
        }
    }

    public void Remove(IZLinkSessionContext context) =>
        ((ICollection<KeyValuePair<string, IZLinkSessionContext>>)_consoles).Remove(
            new KeyValuePair<string, IZLinkSessionContext>(context.SessionId, context));

    public async ValueTask BroadcastAsync(
        NodeStatusNotify message,
        CancellationToken cancellationToken)
    {
        IZLinkSessionContext[] consoles;
        lock (_nodeGate)
        {
            _latestNodes[message.NodeId] = message;
            consoles = _consoles.Values.ToArray();
        }

        await SendAllAsync(consoles, message, cancellationToken);
    }

    public ValueTask BroadcastAsync<TMessage>(
        TMessage message,
        CancellationToken cancellationToken)
    {
        return SendAllAsync(_consoles.Values.ToArray(), message, cancellationToken);
    }

    private async ValueTask SendAllAsync<TMessage>(
        IReadOnlyList<IZLinkSessionContext> consoles,
        TMessage message,
        CancellationToken cancellationToken)
    {
        foreach (var console in consoles)
        {
            try
            {
                await SendAsync(console, message, cancellationToken);
            }
            catch (Exception error)
            {
                Remove(console);
                // A stream can disconnect between the snapshot above and this send. The
                // disconnected console is no longer an observer, so remove it and keep the
                // node/alert broadcast alive for the remaining consoles. Propagating this
                // expected transport race would stop the Ops background event handler and
                // make the next node restart observe a false Ops outage.
                logger?.LogWarning(
                    error,
                    "ops console push dropped. session={SessionId}, message={MessageType}",
                    console.SessionId,
                    typeof(TMessage).Name);
            }
        }
    }

    private static async ValueTask SendAsync<TMessage>(
        IZLinkSessionContext context,
        TMessage message,
        CancellationToken cancellationToken) =>
        await context.Client.Send(message).Async(cancellationToken);
}
