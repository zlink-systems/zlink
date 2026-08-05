namespace Zlink.Framework.Contracts.Streams;

public sealed class ZLinkSessionDispatchContext
{
    private int _replyClaimed;
    public ZLinkSessionDispatchContext(
        string packetName,
        ZLinkMessageMetadata? metadata = null,
        bool canReply = false)
        : this(
            packetName,
            metadata ?? ZLinkMessageMetadata.Empty,
            canReply,
            null)
    {
    }

    internal ZLinkSessionDispatchContext(
        string packetName,
        ZLinkMessageMetadata metadata,
        bool canReply,
        object? runtimeState)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(packetName);
        ArgumentNullException.ThrowIfNull(metadata);
        PacketName = packetName;
        Metadata = metadata;
        CanReply = canReply;
        RuntimeState = runtimeState;
    }

    public string PacketName { get; }

    public ZLinkMessageMetadata Metadata { get; }

    public bool CanReply { get; }

    internal object? RuntimeState { get; }

    internal bool TryClaimReply()
    {
        return CanReply && Interlocked.CompareExchange(ref _replyClaimed, 1, 0) == 0;
    }
}
