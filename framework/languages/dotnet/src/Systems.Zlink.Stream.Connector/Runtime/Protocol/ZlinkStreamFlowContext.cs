namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal static class ZlinkStreamFlowContext
{
    private static readonly AsyncLocal<FlowLease?> Ambient = new();

    public static (string FlowId, ZlinkStreamFlowOrigin Origin)? Current
    {
        get
        {
            var lease = Ambient.Value;
            return lease is { Active: true }
                ? (lease.FlowId, lease.Origin)
                : null;
        }
    }

    public static IDisposable Enter(string? flowId, ZlinkStreamFlowOrigin? origin)
    {
        var previous = Ambient.Value;
        var current = flowId is not null && origin is not null
            ? new FlowLease(flowId, origin.Value)
            : null;
        Ambient.Value = current;
        return new Scope(previous, current);
    }

    public static IDisposable EnterNew(ZlinkStreamFlowOrigin origin) =>
        Enter(ZlinkStreamFlowId.Create(), origin);

    private sealed class FlowLease(string flowId, ZlinkStreamFlowOrigin origin)
    {
        public string FlowId { get; } = flowId;

        public ZlinkStreamFlowOrigin Origin { get; } = origin;

        public bool Active { get; set; } = true;
    }

    private sealed class Scope(FlowLease? previous, FlowLease? current) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            if (current is not null) current.Active = false;
            if (ReferenceEquals(Ambient.Value, current)) Ambient.Value = previous;
        }
    }
}
