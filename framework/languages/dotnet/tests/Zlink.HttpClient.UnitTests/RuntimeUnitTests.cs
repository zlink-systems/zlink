/* SPDX-License-Identifier: Apache-2.0 */

using System.IO.Compression;
using System.Net;
using System.Text;
using Xunit;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient.Runtime;

namespace Zlink.HttpClient.UnitTests;

/// <summary>Direct unit tests for internal runtime helpers (cookie jar, provider stream, compression).</summary>
public sealed class RuntimeUnitTests
{
    [Fact]
    public void CookieJar_stores_and_matches_by_path_and_host()
    {
        var jar = new CookieJar();
        jar.Store("api.test", "sid=abc; Path=/secure");
        jar.Store("api.test", "root=1");
        jar.Store("other.test", "x=9");

        Assert.Equal("root=1", jar.HeaderFor("api.test", "/", false));
        Assert.Contains("sid=abc", jar.HeaderFor("api.test", "/secure/data", false));
        Assert.DoesNotContain("sid", jar.HeaderFor("api.test", "/", false));
        Assert.Equal("x=9", jar.HeaderFor("other.test", "/", false));
    }

    [Fact]
    public void CookieJar_secure_cookie_only_on_secure_requests()
    {
        var jar = new CookieJar();
        jar.Store("api.test", "tok=1; Secure");

        Assert.Equal(string.Empty, jar.HeaderFor("api.test", "/", false));
        Assert.Equal("tok=1", jar.HeaderFor("api.test", "/", true));
    }

    [Fact]
    public void CookieJar_max_age_zero_deletes_and_overwrites()
    {
        var jar = new CookieJar();
        jar.Store("api.test", "a=1");
        jar.Store("api.test", "a=2"); // overwrite same name+path
        Assert.Equal("a=2", jar.HeaderFor("api.test", "/", false));

        jar.Store("api.test", "a=; Max-Age=0"); // delete
        Assert.Equal(string.Empty, jar.HeaderFor("api.test", "/", false));
    }

    [Fact]
    public void CookieJar_evicts_oldest_beyond_host_limit()
    {
        var jar = new CookieJar();
        for (var i = 0; i < 130; i++) jar.Store("api.test", $"c{i}=v");

        var header = jar.HeaderFor("api.test", "/", false);
        Assert.DoesNotContain("c0=", header); // oldest evicted
        Assert.DoesNotContain("c1=", header);
        Assert.Contains("c129=", header); // newest kept
    }

    [Fact]
    public void CookieJar_ignores_malformed_set_cookie()
    {
        var jar = new CookieJar();
        jar.Store("api.test", ""); // empty
        jar.Store("api.test", "novalue"); // no '='
        jar.Store("api.test", "=novalue"); // empty name
        Assert.Equal(string.Empty, jar.HeaderFor("api.test", "/", false));
    }

    [Fact]
    public void ProviderReadStream_reads_chunks_then_eof_via_all_overloads()
    {
        var chunks = new Queue<byte[]>(new[]
        {
            Encoding.UTF8.GetBytes("alpha"),
            Encoding.UTF8.GetBytes("beta")
        });
        using var stream = new ProviderReadStream(() => chunks.Count > 0 ? chunks.Dequeue() : null);

        var array = new byte[3];
        var first = stream.Read(array, 0, array.Length); // byte[] overload
        Assert.Equal(3, first);
        Assert.Equal("alp", Encoding.UTF8.GetString(array, 0, first));

        using var sink = new MemoryStream();
        stream.CopyTo(sink); // drains via Read(Span)
        Assert.Equal("habeta", Encoding.UTF8.GetString(sink.ToArray()));

        Assert.False(stream.CanSeek);
        Assert.False(stream.CanWrite);
        Assert.True(stream.CanRead);
    }

    [Fact]
    public async Task ProviderReadStream_async_overload_reads()
    {
        var chunks = new Queue<byte[]>(new[] { Encoding.UTF8.GetBytes("xy") });
        await using var stream = new ProviderReadStream(() => chunks.Count > 0 ? chunks.Dequeue() : null);

        var buffer = new byte[8];
        var n = await stream.ReadAsync(buffer, 0, buffer.Length);
        Assert.Equal(2, n);
        Assert.Equal(0, await stream.ReadAsync(buffer.AsMemory()));
    }

    [Fact]
    public void ProviderReadStream_unsupported_members_throw()
    {
        using var stream = new ProviderReadStream(() => null);
        Assert.Throws<NotSupportedException>(() => stream.Length);
        Assert.Throws<NotSupportedException>(() => stream.Position);
        Assert.Throws<NotSupportedException>(() => stream.Position = 0);
        Assert.Throws<NotSupportedException>(() => stream.Seek(0, SeekOrigin.Begin));
        Assert.Throws<NotSupportedException>(() => stream.SetLength(1));
        Assert.Throws<NotSupportedException>(() => stream.Write(new byte[1], 0, 1));
        stream.Flush(); // no-op
    }

    [Fact]
    public void ResponseCompression_round_trips_gzip_and_deflate()
    {
        var payload = Encoding.UTF8.GetBytes("the quick brown fox");

        var gz = GzipBytes(payload);
        Assert.Equal(payload, ResponseCompression.Gunzip(gz, 1024));

        var df = DeflateBytes(payload);
        Assert.Equal(payload, ResponseCompression.InflateDeflate(df, 1024));
    }

    [Fact]
    public void ResponseCompression_enforces_decoded_limit()
    {
        var payload = Encoding.UTF8.GetBytes(new string('a', 4096));
        var gz = GzipBytes(payload);

        var ex = Assert.Throws<ZLinkFrameworkException>(() => ResponseCompression.Gunzip(gz, 16));
        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, ex.Kind);
    }

    [Fact]
    public void HttpHeaderLookup_finds_and_removes_headers_case_insensitively()
    {
        IReadOnlyDictionary<string, string> headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["Content-Type"] = "application/json",
            ["CONTENT-LENGTH"] = "42",
            ["x-marker"] = "keep"
        };

        Assert.Equal("application/json", HttpHeaderLookup.Find(headers, "content-type"));
        Assert.Null(HttpHeaderLookup.Find(headers, "missing"));

        var stripped = HttpHeaderLookup.Without(headers, "content-length", "content-encoding");
        Assert.False(stripped.ContainsKey("CONTENT-LENGTH"));
        Assert.Equal("keep", stripped["x-marker"]);
    }

    [Fact]
    public async Task Proxy_credentials_are_not_added_to_origin_request_headers()
    {
        var handler = new CapturingRequestHandler();
        using var httpClient = new System.Net.Http.HttpClient(handler);
        var options = CreateProxyOptions();
        var performer = new RequestPerformer(options, new CookieJar(), httpClient);
        var request = new HttpRequestSpec
        {
            Method = ZLinkHttpMethod.Get,
            Target = "/resource",
            Headers = new Dictionary<string, string>()
        };

        _ = await performer.PerformAsync(request, CancellationToken.None);

        Assert.Null(handler.ProxyAuthorization);
    }

    [Fact]
    public void Proxy_credentials_are_configured_on_transport_proxy()
    {
        using var transport = HttpTransportFactory.Create(CreateProxyOptions());

        var proxy = Assert.IsType<WebProxy>(transport.Handler.Proxy);
        var credentials = Assert.IsType<NetworkCredential>(proxy.Credentials);
        Assert.Equal("proxy-user", credentials.UserName);
        Assert.Equal("proxy-password", credentials.Password);
    }

    private static HttpClientOptions CreateProxyOptions()
    {
        return new HttpClientOptions
        {
            BaseUrl = "https://origin.test",
            Headers = new Dictionary<string, string>(),
            Codecs = new HttpClientCodecRegistry(),
            Proxy = "http://proxy.test:3128",
            ProxyCredentials = ("proxy-user", "proxy-password")
        };
    }

    private sealed class CapturingRequestHandler : HttpMessageHandler
    {
        public string? ProxyAuthorization { get; private set; }

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            ProxyAuthorization = request.Headers.TryGetValues("Proxy-Authorization", out var values)
                ? values.Single()
                : null;
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new ByteArrayContent([])
            });
        }
    }

    private static byte[] GzipBytes(byte[] input)
    {
        using var output = new MemoryStream();
        using (var gz = new GZipStream(output, CompressionMode.Compress, true))
        {
            gz.Write(input, 0, input.Length);
        }

        return output.ToArray();
    }

    private static byte[] DeflateBytes(byte[] input)
    {
        using var output = new MemoryStream();
        using (var df = new ZLibStream(output, CompressionMode.Compress, true))
        {
            df.Write(input, 0, input.Length);
        }

        return output.ToArray();
    }
}
