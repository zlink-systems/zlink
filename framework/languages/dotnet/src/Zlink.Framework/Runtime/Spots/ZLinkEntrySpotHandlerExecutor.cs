namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkEntrySpotHandlerExecutor(
    ZLinkSpotHandlerInvoker invoker)
{
    public async ValueTask InvokeActorPacketAsync(
        ZLinkSpotActorPacketDescriptor descriptor,
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken)
    {
        await invoker.InvokeActorPacketAsync(
                descriptor,
                actor,
                header,
                body,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkActorReply> InvokeActorPacketForReplyAsync(
        ZLinkSpotActorPacketDescriptor descriptor,
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken)
    {
        return await invoker.InvokeActorPacketForReplyAsync(
                descriptor,
                actor,
                header,
                body,
                cancellationToken)
            .ConfigureAwait(false);
    }
}
