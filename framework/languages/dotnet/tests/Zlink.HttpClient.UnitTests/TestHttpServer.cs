/* SPDX-License-Identifier: Apache-2.0 */

using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;

namespace Zlink.HttpClient.UnitTests;

/// <summary>
///     Minimal in-process HTTP server backed by Kestrel for contract tests.
///     Mirrors the role of the C++ <c>test_cpp_http_client</c> embedded server.
/// </summary>
internal sealed class TestHttpServer : IDisposable
{
    private readonly WebApplication _app;

    public TestHttpServer(Func<HttpContext, Task> handler)
    {
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseKestrel(options => options.Listen(System.Net.IPAddress.Loopback, 0));
        _app = builder.Build();
        _app.Run(context => handler(context));
        _app.StartAsync().GetAwaiter().GetResult();
        BaseUrl = _app.Urls.Single();
    }

    public string BaseUrl { get; }

    public void Dispose()
    {
        _app.DisposeAsync().AsTask().GetAwaiter().GetResult();
    }
}

/// <summary>Helpers for writing Kestrel responses in tests.</summary>
internal static class TestHttpServerExtensions
{
    public static async Task WriteAsync(
        this HttpResponse response,
        int status,
        string body,
        string contentType = "application/json"
    )
    {
        response.StatusCode = status;
        response.ContentType = contentType;
        var bytes = Encoding.UTF8.GetBytes(body);
        response.ContentLength = bytes.Length;
        await response.Body.WriteAsync(bytes).ConfigureAwait(false);
    }

    public static async Task<string> ReadBodyAsync(this HttpRequest request)
    {
        using var reader = new StreamReader(request.Body, Encoding.UTF8);
        return await reader.ReadToEndAsync().ConfigureAwait(false);
    }

    public static async Task<byte[]> ReadBodyBytesAsync(this HttpRequest request)
    {
        using var output = new MemoryStream();
        await request.Body.CopyToAsync(output).ConfigureAwait(false);
        return output.ToArray();
    }

    public static async Task WriteBytesAsync(
        this HttpResponse response,
        int status,
        byte[] body,
        string contentType
    )
    {
        response.StatusCode = status;
        response.ContentType = contentType;
        response.ContentLength = body.Length;
        await response.Body.WriteAsync(body).ConfigureAwait(false);
    }
}
