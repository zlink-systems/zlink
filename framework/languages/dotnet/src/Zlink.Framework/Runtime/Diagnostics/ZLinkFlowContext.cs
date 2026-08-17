using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Diagnostics;

internal static class ZLinkFlowContext
{
    private static readonly AsyncLocal<State?> Ambient = new();

    public static ZLinkFlowValue? Current
    {
        get
        {
            var state = Ambient.Value;
            return state is { Active: true } ? state.Value : null;
        }
    }

    public static Scope Enter(
        string? flowId,
        ZLinkFlowOrigin? origin,
        bool createIfAbsent,
        ZLinkFlowOrigin defaultOrigin)
    {
        return EnterEnabled(flowId, origin, createIfAbsent, defaultOrigin);
    }

    public static Scope EnterExisting(
        string? flowId,
        ZLinkFlowOrigin? origin)
    {
        return EnterEnabled(flowId, origin, createIfAbsent: false, default);
    }

    private static Scope EnterEnabled(
        string? flowId,
        ZLinkFlowOrigin? origin,
        bool createIfAbsent,
        ZLinkFlowOrigin defaultOrigin)
    {
        if ((flowId is null) != (origin is null))
            throw new InvalidOperationException("Flow id and origin must be present together.");

        ZLinkFlowValue? value = null;
        if (flowId is not null)
        {
            if (!ZlinkStreamFlowId.IsValid(flowId))
                throw new InvalidOperationException("Flow id must be UUIDv7.");
            value = new ZLinkFlowValue(flowId, origin!.Value);
        }
        else if (createIfAbsent)
        {
            value = new ZLinkFlowValue(ZlinkStreamFlowId.Create(), defaultOrigin);
        }

        var previous = Ambient.Value;
        var state = value is null ? null : new State(value.Value);
        Ambient.Value = state;
        return new Scope(previous, state);
    }

    public static Scope EnterCurrentOrCreate(ZLinkFlowOrigin origin, bool createIfAbsent)
    {
        // Off disables creation of a new flow, not propagation of an existing
        // ambient flow. A no-op scope preserves that flow without allocating a
        // replacement State.
        if (!createIfAbsent) return default;

        var current = Current;
        return EnterEnabled(current?.FlowId, current?.Origin, createIfAbsent: true, origin);
    }

    internal sealed class State(ZLinkFlowValue value)
    {
        public bool Active { get; set; } = true;

        public ZLinkFlowValue Value { get; } = value;
    }

    internal readonly struct Scope(
        State? previous,
        State? entered,
        bool restore = true) : IDisposable
    {
        public void Dispose()
        {
            if (!restore) return;
            if (entered is not null) entered.Active = false;
            Ambient.Value = previous;
        }
    }
}

internal readonly record struct ZLinkFlowValue(string FlowId, ZLinkFlowOrigin Origin);
