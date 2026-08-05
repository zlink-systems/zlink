namespace Systems.Zlink.Stream.Connector.Runtime.Calls;

internal sealed class ZlinkStreamCallBuilderState(string? name)
{
    private int _executed;

    public string? Name { get; private set; } = name;

    public ZlinkStreamMetadata Metadata { get; private set; } = ZlinkStreamMetadata.Empty;

    public TimeSpan? Timeout { get; private set; }

    public bool Compress { get; private set; }

    public void SetMessageName(string name)
    {
        Name = name;
    }

    public void AddMetadata(string key, string value)
    {
        Metadata = Metadata.With(key, value);
    }

    public void SetMetadata(ZlinkStreamMetadata metadata)
    {
        Metadata = metadata ?? throw new ArgumentNullException(nameof(metadata));
    }

    public void SetTimeout(TimeSpan timeout)
    {
        Timeout = timeout;
    }

    public void EnableCompression()
    {
        Compress = true;
    }

    public void EnsureNotExecuted()
    {
        if (Interlocked.Exchange(ref _executed, 1) != 0)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Builder instances can be executed only once.");
    }

    public string ResolveMessageName()
    {
        return Name
               ?? throw ZlinkStreamConnector.Error(
                   ZlinkStreamErrorCode.ValidationFailed,
                   "Message name is required when the encoded stream payload has no message type.");
    }
}
