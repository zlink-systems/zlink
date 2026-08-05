/* SPDX-License-Identifier: Apache-2.0 */

using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Zlink.HttpClient.UnitTests;

/// <summary>
///     Raw-socket server that deterministically fails the first <c>failCount</c> connections (accept
///     then close, producing a transport error on the client) and answers 200 afterwards. Used to test
///     retry on retriable transport failures without depending on <see cref="HttpListener" /> abort
///     semantics, which are unreliable on the managed Linux listener.
/// </summary>
internal sealed class FlakyRawServer : IDisposable
{
    private readonly CancellationTokenSource _cts = new();
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private int _connectionCount;

    public FlakyRawServer(int failCount)
    {
        _listener.Start();
        var port = ((IPEndPoint)_listener.LocalEndpoint).Port;
        BaseUrl = $"http://127.0.0.1:{port}";
        _ = AcceptLoopAsync(failCount);
    }

    public string BaseUrl { get; }

    public int ConnectionCount => Volatile.Read(ref _connectionCount);

    public void Dispose()
    {
        _cts.Cancel();
        try
        {
            _listener.Stop();
        }
        catch
        {
            // already stopped
        }

        _cts.Dispose();
    }

    private async Task AcceptLoopAsync(int failCount)
    {
        while (!_cts.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await _listener.AcceptTcpClientAsync(_cts.Token).ConfigureAwait(false);
            }
            catch
            {
                return;
            }

            var index = Interlocked.Increment(ref _connectionCount);
            _ = Task.Run(async () =>
            {
                using (client)
                {
                    if (index <= failCount)
                    {
                        // Drop the connection abruptly to force a retriable transport error.
                        client.LingerState = new LingerOption(true, 0);
                        return;
                    }

                    var stream = client.GetStream();
                    var buffer = new byte[4096];
                    await stream.ReadAsync(buffer).ConfigureAwait(false);
                    var response = Encoding.ASCII.GetBytes(
                        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                    await stream.WriteAsync(response).ConfigureAwait(false);
                    await stream.FlushAsync().ConfigureAwait(false);
                }
            });
        }
    }
}