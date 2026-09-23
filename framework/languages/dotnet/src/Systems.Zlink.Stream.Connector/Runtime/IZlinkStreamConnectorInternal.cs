namespace Systems.Zlink.Stream.Connector.Runtime;

internal interface IZlinkStreamConnectorInternal : IZlinkStreamConnector
{
    ZlinkStreamOutboundFrame BuildSendFrame(
        ZlinkStreamMessageKind kind,
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        ushort? actorSlot
    );

    ValueTask SendFrameAsync(ZlinkStreamOutboundFrame frame, CancellationToken cancellationToken);

    ValueTask SubmitFrameAsync(ZlinkStreamOutboundFrame frame, CancellationToken cancellationToken);

    ValueTask<ZlinkStreamEncodedPayload> RequestEncodedAsync(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        ushort? actorSlot,
        string? actorId,
        CancellationToken cancellationToken
    );

    void RequestEncoded(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        ushort? actorSlot,
        string? actorId,
        Action<ZlinkStreamResult> callback
    );

    void RequestEncoded(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        ushort? actorSlot,
        string? actorId,
        Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback
    );

    /// <summary>
    ///     Consumes the next matching unread message, or yields <see langword="null" />
    ///     when the timeout elapses first. Each wait surface turns that outcome into its
    ///     own <see cref="ZlinkStreamErrorCode.ValidationFailed" /> result.
    /// </summary>
    ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>?> WaitForEncodedAsync(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken
    );
}
