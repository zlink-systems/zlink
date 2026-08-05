using System.Net.Sockets;

namespace Systems.Zlink.Stream.Connector.Runtime.Transport;

internal sealed class StreamConnection(TcpClient tcpClient, System.IO.Stream stream) : IZlinkStreamConnection
{
    public bool CanWriteSegments => true;

    public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
    {
        return await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken)
    {
        await stream.WriteAsync(buffer, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask CloseAsync(CancellationToken cancellationToken)
    {
        try
        {
            await stream.DisposeAsync().ConfigureAwait(false);
        }
        finally
        {
            tcpClient.Dispose();
        }
    }
}
