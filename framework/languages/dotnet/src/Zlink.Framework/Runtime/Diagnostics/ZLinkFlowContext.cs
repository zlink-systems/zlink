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
        bool captureEnabled,
        ZLinkFlowOrigin defaultOrigin,
        bool createIfAbsent = true)
    {
        // Flow fields are observation-only. At Off the processing point must
        // neither validate nor install inbound flow state, create a new flow,
        // nor copy any flow forward onto outbound envelopes (spec 27 §4).
        if (!captureEnabled) return SuppressAmbient();

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

    public static Scope EnterCurrentOrCreate(ZLinkFlowOrigin origin, bool captureEnabled)
    {
        if (!captureEnabled) return SuppressAmbient();

        var current = Current;
        return EnterEnabled(current?.FlowId, current?.Origin, createIfAbsent: true, origin);
    }

    // Off path: no validation, no context capture, no new flow (spec 27 §4).
    // A stale ambient flow left by a scope entered while tracing was enabled
    // is cleared for the duration so outbound encoders do not copy it onto
    // envelopes; steady-state Off pays only the ambient null read.
    private static Scope SuppressAmbient()
    {
        var previous = Ambient.Value;
        if (previous is not { Active: true }) return default;

        Ambient.Value = null;
        return new Scope(previous, null);
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
