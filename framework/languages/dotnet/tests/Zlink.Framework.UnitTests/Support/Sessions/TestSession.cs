namespace Zlink.Framework.UnitTests;

internal sealed class TestSession : IZLinkSession
{
    public IZLinkSessionContext Context => null!;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}
