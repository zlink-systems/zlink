namespace Systems.Zlink.Stream.Connector.Runtime;

internal interface IZlinkStreamConnectorInternal : IZlinkStreamConnector
{
    ZlinkStreamOutboundFrame BuildSendFrame(
        ZlinkStreamMessageKind kind,
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress);

    ValueTask SendFrameAsync(
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken);

    ValueTask SubmitFrameAsync(
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken);

    ValueTask<ZlinkStreamEncodedPayload> RequestEncodedAsync(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        CancellationToken cancellationToken);

    void RequestEncoded(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        Action<ZlinkStreamResult> callback);

    void RequestEncoded(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback);

    ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> WaitForEncodedAsync(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken);
}
