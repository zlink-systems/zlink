/* SPDX-License-Identifier: Apache-2.0 */

using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Zlink.HttpClient.UnitTests;

/// <summary>
///     Minimal raw-socket HTTP/1.1 server used where <see cref="HttpListener" /> cannot help — notably
///     reading a chunked (<c>Transfer-Encoding: chunked</c>) request body, which the managed Linux
///     <see cref="HttpListener" /> does not support. It reads one request, decodes a chunked or
///     content-length body, records it, and replies with a fixed 200 response.
/// </summary>
internal sealed class RawCaptureServer : IDisposable
{
    private readonly TaskCompletionSource<string> _captured = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TcpListener _listener;

    public RawCaptureServer(string responseBody = "{}")
    {
        _listener = new TcpListener(IPAddress.Loopback, 0);
        _listener.Start();
        var port = ((IPEndPoint)_listener.LocalEndpoint).Port;
        BaseUrl = $"http://127.0.0.1:{port}";
        _ = ServeAsync(responseBody);
    }

    public string BaseUrl { get; }

    /// <summary>Completes with the decoded request body once a request has been served.</summary>
    public Task<string> CapturedBody => _captured.Task;

    public void Dispose()
    {
        try
        {
            _listener.Stop();
        }
        catch
        {
            // already stopped
        }
    }

    private async Task ServeAsync(string responseBody)
    {
        try
        {
            using var connection = await _listener.AcceptTcpClientAsync().ConfigureAwait(false);
            await using var stream = connection.GetStream();

            var headerText = await ReadHeadersAsync(stream).ConfigureAwait(false);
            var chunked = headerText.Contains("transfer-encoding: chunked", StringComparison.OrdinalIgnoreCase);
            var contentLength = ParseContentLength(headerText);

            var body = chunked
                ? await ReadChunkedAsync(stream).ConfigureAwait(false)
                : await ReadFixedAsync(stream, contentLength).ConfigureAwait(false);

            var payload = Encoding.UTF8.GetBytes(responseBody);
            var response = Encoding.UTF8.GetBytes(
                $"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {payload.Length}\r\nConnection: close\r\n\r\n");
            await stream.WriteAsync(response).ConfigureAwait(false);
            await stream.WriteAsync(payload).ConfigureAwait(false);
            await stream.FlushAsync().ConfigureAwait(false);

            _captured.TrySetResult(body);
        }
        catch (Exception ex)
        {
            _captured.TrySetException(ex);
        }
    }

    private static async Task<string> ReadHeadersAsync(NetworkStream stream)
    {
        var buffer = new List<byte>();
        var one = new byte[1];
        while (true)
        {
            var read = await stream.ReadAsync(one).ConfigureAwait(false);
            if (read == 0) break;

            buffer.Add(one[0]);
            if (buffer.Count >= 4
                && buffer[^4] == (byte)'\r' && buffer[^3] == (byte)'\n'
                && buffer[^2] == (byte)'\r' && buffer[^1] == (byte)'\n')
                break;
        }

        return Encoding.ASCII.GetString(buffer.ToArray());
    }

    private static int ParseContentLength(string headers)
    {
        foreach (var line in headers.Split("\r\n"))
            if (line.StartsWith("content-length:", StringComparison.OrdinalIgnoreCase))
                return int.Parse(line["content-length:".Length..].Trim());

        return 0;
    }

    private static async Task<string> ReadFixedAsync(NetworkStream stream, int length)
    {
        var buffer = new byte[length];
        var offset = 0;
        while (offset < length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(offset)).ConfigureAwait(false);
            if (read == 0) break;

            offset += read;
        }

        return Encoding.UTF8.GetString(buffer, 0, offset);
    }

    private static async Task<string> ReadChunkedAsync(NetworkStream stream)
    {
        var body = new StringBuilder();
        while (true)
        {
            var sizeLine = await ReadLineAsync(stream).ConfigureAwait(false);
            var size = Convert.ToInt32(sizeLine.Split(';')[0].Trim(), 16);
            if (size == 0)
            {
                await ReadLineAsync(stream).ConfigureAwait(false); // trailing CRLF
                break;
            }

            var chunk = await ReadFixedAsync(stream, size).ConfigureAwait(false);
            body.Append(chunk);
            await ReadLineAsync(stream).ConfigureAwait(false); // CRLF after chunk data
        }

        return body.ToString();
    }

    private static async Task<string> ReadLineAsync(NetworkStream stream)
    {
        var buffer = new List<byte>();
        var one = new byte[1];
        while (true)
        {
            var read = await stream.ReadAsync(one).ConfigureAwait(false);
            if (read == 0) break;

            if (one[0] == (byte)'\n') break;

            if (one[0] != (byte)'\r') buffer.Add(one[0]);
        }

        return Encoding.ASCII.GetString(buffer.ToArray());
    }
}