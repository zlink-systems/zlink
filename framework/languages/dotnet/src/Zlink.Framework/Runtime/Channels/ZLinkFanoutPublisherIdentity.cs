using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkFanoutPublisherIdentity(
    string channelName,
    RoutingId publisherRid,
    ulong lifecycleGeneration,
    string endpoint)
{
    private readonly ZLinkStateLane _lane = new();
    private ulong _descriptorRevision = 1;
    private ZLinkFrameworkRuntimeState _state =
        ZLinkFrameworkRuntimeState.Serving;

    internal string ChannelName { get; } = channelName;
    internal RoutingId PublisherRid { get; } = publisherRid;
    internal ulong LifecycleGeneration { get; } = lifecycleGeneration;
    internal string Endpoint { get; } = endpoint;

    internal ValueTask<Snapshot> ReadAsync() =>
        _lane.RunAsync(() => new Snapshot(_descriptorRevision, _state));

    internal ValueTask<Snapshot> MarkDrainingAsync() =>
        SetStateAsync(ZLinkFrameworkRuntimeState.Draining);

    internal ValueTask<Snapshot> MarkRetiringAsync() =>
        SetStateAsync(ZLinkFrameworkRuntimeState.Relocating);

    internal ValueTask<Snapshot> MarkServingAsync() =>
        SetStateAsync(ZLinkFrameworkRuntimeState.Serving);

    private ValueTask<Snapshot> SetStateAsync(ZLinkFrameworkRuntimeState state) =>
        _lane.RunAsync(() =>
        {
            _descriptorRevision++;
            _state = state;
            return new Snapshot(_descriptorRevision, _state);
        });

    internal Snapshot Read() => AwaitStateLane(ReadAsync());

    internal Snapshot MarkDraining() => AwaitStateLane(MarkDrainingAsync());

    internal Snapshot MarkRetiring() => AwaitStateLane(MarkRetiringAsync());

    internal Snapshot MarkServing() => AwaitStateLane(MarkServingAsync());

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    internal readonly record struct Snapshot(
        ulong DescriptorRevision,
        ZLinkFrameworkRuntimeState State);
}
