namespace Systems.Zlink.Stream.Connector.Runtime.Calls;

internal sealed class ZlinkStreamLifecycleCall(Func<CancellationToken, ValueTask> execute)
    : IZlinkStreamLifecycleCall
{
    public ValueTask Async(CancellationToken cancellationToken = default)
    {
        return execute(cancellationToken);
    }
}