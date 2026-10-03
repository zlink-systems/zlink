namespace Systems.Zlink.Stream.Connector.Runtime.Transport;

internal interface IZlinkStreamConnection
{
    ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken);

    ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken);

    ValueTask CloseAsync(CancellationToken cancellationToken);
}
