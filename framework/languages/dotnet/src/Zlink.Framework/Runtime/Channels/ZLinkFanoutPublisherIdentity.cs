namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkFanoutPublisherIdentity(
    string channelName,
    RoutingId publisherRid,
    ulong lifecycleGeneration,
    string endpoint)
{
    private readonly object _gate = new();
    private ulong _descriptorRevision = 1;
    private ZLinkFrameworkRuntimeState _state =
        ZLinkFrameworkRuntimeState.Serving;

    internal string ChannelName { get; } = channelName;
    internal RoutingId PublisherRid { get; } = publisherRid;
    internal ulong LifecycleGeneration { get; } = lifecycleGeneration;
    internal string Endpoint { get; } = endpoint;

    internal Snapshot Read()
    {
        lock (_gate)
            return new Snapshot(_descriptorRevision, _state);
    }

    internal Snapshot MarkDraining()
        => SetState(ZLinkFrameworkRuntimeState.Draining);

    internal Snapshot MarkRetiring()
        => SetState(ZLinkFrameworkRuntimeState.Relocating);

    internal Snapshot MarkServing()
        => SetState(ZLinkFrameworkRuntimeState.Serving);

    private Snapshot SetState(ZLinkFrameworkRuntimeState state)
    {
        lock (_gate)
        {
            _descriptorRevision++;
            _state = state;
            return new Snapshot(_descriptorRevision, _state);
        }
    }

    internal readonly record struct Snapshot(
        ulong DescriptorRevision,
        ZLinkFrameworkRuntimeState State);
}
