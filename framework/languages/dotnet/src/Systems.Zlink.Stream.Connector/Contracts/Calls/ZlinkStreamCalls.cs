namespace Systems.Zlink.Stream.Connector.Contracts.Calls;

public interface IZlinkStreamLifecycleCall
{
    ValueTask Async(CancellationToken cancellationToken = default);
}

public interface IZlinkStreamSendCall
{
    IZlinkStreamSendCall PacketName(string name);

    IZlinkStreamSendCall Metadata(string key, string value);

    IZlinkStreamSendCall Metadata(ZlinkStreamMetadata metadata);

    IZlinkStreamSendCall Compress();

    ValueTask Async(CancellationToken cancellationToken = default);
}

public interface IZlinkStreamRequestCall
{
    IZlinkStreamRequestCall PacketName(string name);

    IZlinkStreamRequestCall Metadata(string key, string value);

    IZlinkStreamRequestCall Metadata(ZlinkStreamMetadata metadata);

    IZlinkStreamRequestCall Timeout(TimeSpan timeout);

    IZlinkStreamRequestCall Compress();

    ValueTask<ZlinkStreamEncodedPayload> Async(CancellationToken cancellationToken = default);

    void Submit(Action<ZlinkStreamResult> callback);

    void Submit(Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback);
}

public interface IZlinkStreamWaitCall
{
    IZlinkStreamWaitCall Timeout(TimeSpan timeout);

    IZlinkStreamWaitCall Where(Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate);

    ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> Async(
        CancellationToken cancellationToken = default);
}

/// <summary>
///     Configures and executes a negative observation for one packet name.
/// </summary>
public interface IZlinkStreamExpectNoneCall
{
    /// <summary>
    ///     Sets the interval during which the packet must not arrive.
    /// </summary>
    IZlinkStreamExpectNoneCall Within(TimeSpan window);

    /// <summary>
    ///     Executes the negative observation.
    /// </summary>
    ValueTask Async(CancellationToken cancellationToken = default);
}

/// <summary>
///     Configures and executes ordered expectations for one packet name.
/// </summary>
public interface IZlinkStreamSequenceCall
{
    /// <summary>
    ///     Appends the next expected message predicate.
    /// </summary>
    IZlinkStreamSequenceCall Expect(
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate);

    /// <summary>
    ///     Sets the total timeout for the complete sequence.
    /// </summary>
    IZlinkStreamSequenceCall Timeout(TimeSpan timeout);

    /// <summary>
    ///     Verifies the sequence and returns the consumed messages in arrival order.
    /// </summary>
    ValueTask<IReadOnlyList<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>> Async(
        CancellationToken cancellationToken = default);
}
