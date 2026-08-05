/* SPDX-License-Identifier: Apache-2.0 */

using System.Diagnostics;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using Google.Protobuf;
using Google.Protobuf.WellKnownTypes;
using MessagePack;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Codecs;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient.Runtime;

namespace Zlink.HttpClient.UnitTests;

public sealed class HttpClientContractTests
{
    [Theory]
    [InlineData("GET")]
    [InlineData("POST")]
    [InlineData("PUT")]
    [InlineData("DELETE")]
    [InlineData("PATCH")]
    [InlineData("OPTIONS")]
    public async Task Dispatches_each_method(string method)
    {
        string? seen = null;
        using var server = new TestHttpServer(async ctx =>
        {
            seen = ctx.Request.HttpMethod;
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var request = method switch
        {
            "GET" => client.Get("/r"),
            "POST" => client.Post("/r"),
            "PUT" => client.Put("/r"),
            "DELETE" => client.Delete("/r"),
            "PATCH" => client.Patch("/r"),
            _ => client.Options("/r")
        };
        var response = await request.AsyncRaw();

        Assert.Equal(200, response.Status);
        Assert.Equal(method, seen);
    }

    [Fact]
    public async Task Head_returns_status_with_empty_body()
    {
        using var server = new TestHttpServer(async ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.AddHeader("x-marker", "present");
            await Task.CompletedTask;
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var response = await client.Head("/r").AsyncRaw();

        Assert.Equal(200, response.Status);
        Assert.Equal(string.Empty, response.Body);
        Assert.Equal("present", response.Headers["x-marker"]);
    }

    [Fact]
    public async Task Percent_encodes_query_parameters()
    {
        string? rawUrl = null;
        using var server = new TestHttpServer(async ctx =>
        {
            rawUrl = ctx.Request.Url!.PathAndQuery;
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        await client.Get("/search").Query("q", "a b&c").Query("k", "v/w").AsyncRaw();

        Assert.Equal("/search?q=a%20b%26c&k=v%2Fw", rawUrl);
    }

    [Fact]
    public async Task Sends_default_and_request_headers()
    {
        string? auth = null;
        string? trace = null;
        using var server = new TestHttpServer(async ctx =>
        {
            auth = ctx.Request.Headers["authorization"];
            trace = ctx.Request.Headers["x-trace"];
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl)
            .DefaultHeader("authorization", "Bearer token-123")
            .Build();

        await client.Get("/r").Header("x-trace", "abc").AsyncRaw();

        Assert.Equal("Bearer token-123", auth);
        Assert.Equal("abc", trace);
    }

    [Fact]
    public async Task Typed_json_round_trip()
    {
        CreateGameReq? received = null;
        using var server = new TestHttpServer(async ctx =>
        {
            received = JsonSerializer.Deserialize<CreateGameReq>(
                ctx.Request.ReadBody(), new JsonSerializerOptions(JsonSerializerDefaults.Web));
            await ctx.Response.WriteAsync(200, """{"id":"game-7","ranked":true}""");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var response = await client.Post("/games").Body(new CreateGameReq("ranked-0611")).Async<CreateGameRes>();

        Assert.Equal("ranked-0611", received!.Name);
        Assert.Equal("game-7", response.Body.Id);
        Assert.True(response.Body.Ranked);
    }

    [Fact]
    public async Task Protobuf_extension_round_trips_typed_body_and_response()
    {
        string? contentType = null;
        StringValue? received = null;
        using var server = new TestHttpServer(async ctx =>
        {
            contentType = ctx.Request.ContentType;
            received = StringValue.Parser.ParseFrom(ctx.Request.ReadBodyBytes());
            await ctx.Response.WriteBytesAsync(
                200,
                new StringValue { Value = "pong" }.ToByteArray(),
                "application/x-protobuf");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl)
            .Codecs(codecs => codecs.Use(ZLinkProtobufCodec.Default))
            .Build();

        var response = await client.Post("/proto")
            .Body(new StringValue { Value = "ping" })
            .Async<StringValue>();

        Assert.Equal("application/x-protobuf", contentType);
        Assert.Equal("ping", received!.Value);
        Assert.Equal("pong", response.Body.Value);
    }

    [Fact]
    public async Task MessagePack_extension_round_trips_typed_body_and_response()
    {
        string? contentType = null;
        PackedPlayer? received = null;
        using var server = new TestHttpServer(async ctx =>
        {
            contentType = ctx.Request.ContentType;
            received = MessagePackSerializer.Deserialize<PackedPlayer>(ctx.Request.ReadBodyBytes());
            await ctx.Response.WriteBytesAsync(
                200,
                MessagePackSerializer.Serialize(new PackedPlayer { Id = 9, Name = "reply" }),
                "application/x-msgpack");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl)
            .Codecs(codecs => codecs.Use(ZLinkMessagePackCodec.Default))
            .Build();

        var response = await client.Post("/packed")
            .Body(new PackedPlayer { Id = 7, Name = "request" })
            .Async<PackedPlayer>();

        Assert.Equal("application/x-msgpack", contentType);
        Assert.Equal(7, received!.Id);
        Assert.Equal("request", received.Name);
        Assert.Equal(9, response.Body.Id);
        Assert.Equal("reply", response.Body.Name);
    }

    [Fact]
    public async Task Fetch_returns_typed_body_directly()
    {
        using var server = new TestHttpServer(async ctx =>
            await ctx.Response.WriteAsync(200, """{"id":7,"name":"Aria"}"""));
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var player = await client.Get("/players/7").Fetch<Player>();

        Assert.Equal(7, player.Id);
        Assert.Equal("Aria", player.Name);
    }

    [Fact]
    public async Task Typed_204_returns_null_body()
    {
        using var server = new TestHttpServer(async ctx =>
        {
            ctx.Response.StatusCode = 204;
            await Task.CompletedTask;
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var response = await client.Get("/empty").Async<Player>();

        Assert.Equal(204, response.Status);
        Assert.Null(response.Body);
        Assert.Equal(string.Empty, response.RawBody);
    }

    [Fact]
    public async Task Raw_body_sets_content_type()
    {
        string? contentType = null;
        string? body = null;
        using var server = new TestHttpServer(async ctx =>
        {
            contentType = ctx.Request.ContentType;
            body = ctx.Request.ReadBody();
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        await client.Post("/raw").Body("plain text", "text/plain").AsyncRaw();

        Assert.Equal("plain text", body);
        Assert.StartsWith("text/plain", contentType);
    }

    [Fact]
    public async Task Form_urlencoded_body()
    {
        string? contentType = null;
        string? body = null;
        using var server = new TestHttpServer(async ctx =>
        {
            contentType = ctx.Request.ContentType;
            body = ctx.Request.ReadBody();
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        await client.Post("/form").Form("name", "a b").Form("city", "x/y").AsyncRaw();

        Assert.Equal("application/x-www-form-urlencoded", contentType);
        Assert.Equal("name=a%20b&city=x%2Fy", body);
    }

    [Fact]
    public async Task Multipart_body()
    {
        string? contentType = null;
        string? body = null;
        using var server = new TestHttpServer(async ctx =>
        {
            contentType = ctx.Request.ContentType;
            body = ctx.Request.ReadBody();
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        await client.Post("/upload")
            .Multipart("field", "value")
            .MultipartFile("file", "a.txt", "file-content", "text/plain")
            .AsyncRaw();

        Assert.StartsWith("multipart/form-data; boundary=", contentType);
        Assert.Contains("name=\"field\"", body);
        Assert.Contains("filename=\"a.txt\"", body);
        Assert.Contains("file-content", body);
    }

    [Fact]
    public async Task Streaming_upload_sends_all_chunks()
    {
        // Uses a raw-socket server: the managed Linux HttpListener cannot read a chunked
        // request body, so it cannot validate chunked streaming uploads.
        using var server = new RawCaptureServer();
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var chunks = new Queue<string>(new[] { "part-1;", "part-2;", "part-3" });
        await client.Post("/stream")
            .BodyStream(
                () => chunks.Count > 0 ? Encoding.UTF8.GetBytes(chunks.Dequeue()) : null,
                "application/octet-stream")
            .AsyncRaw();

        Assert.Equal("part-1;part-2;part-3", await server.CapturedBody);
    }

    [Fact]
    public async Task Streaming_download_delivers_chunks_to_sink()
    {
        using var server = new TestHttpServer(async ctx =>
            await ctx.Response.WriteAsync(200, "streamed-response-payload", "text/plain"));
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var sink = new StringBuilder();
        var response = await client.Get("/download")
            .DownloadAsync(chunk => sink.Append(Encoding.UTF8.GetString(chunk.Span)));

        Assert.Equal(200, response.Status);
        Assert.Equal(string.Empty, response.Body);
        Assert.Equal("streamed-response-payload", sink.ToString());
    }

    [Fact]
    public async Task Status_400_or_more_throws_request_failed()
    {
        using var server = new TestHttpServer(async ctx =>
            await ctx.Response.WriteAsync(404, """{"error":"missing"}"""));
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var ex = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await client.Get("/players/0").Async<Player>());

        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, ex.Kind);
    }

    [Fact]
    public async Task Malformed_json_throws_payload_decode_failed()
    {
        using var server = new TestHttpServer(async ctx =>
            await ctx.Response.WriteAsync(200, "not-json"));
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var ex = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await client.Get("/players/7").Async<Player>());

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, ex.Kind);
    }

    [Fact]
    public async Task Follows_redirect_and_rewrites_post_to_get_on_303()
    {
        string? finalMethod = null;
        using var server = new TestHttpServer(async ctx =>
        {
            if (ctx.Request.Url!.AbsolutePath == "/start")
            {
                ctx.Response.StatusCode = 303;
                ctx.Response.AddHeader("Location", "/result");
                return;
            }

            finalMethod = ctx.Request.HttpMethod;
            await ctx.Response.WriteAsync(200, """{"id":"game-1","ranked":false}""");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).FollowRedirects(3).Build();

        var response = await client.Post("/start").Body(new CreateGameReq("x")).Async<CreateGameRes>();

        Assert.Equal("GET", finalMethod);
        Assert.Equal("game-1", response.Body.Id);
    }

    [Fact]
    public async Task Follows_path_relative_redirect()
    {
        string? finalPath = null;
        using var server = new TestHttpServer(async ctx =>
        {
            if (ctx.Request.Url!.AbsolutePath == "/games/start")
            {
                ctx.Response.StatusCode = 307;
                ctx.Response.AddHeader("Location", "result");
                return;
            }

            finalPath = ctx.Request.Url.AbsolutePath;
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).FollowRedirects(3).Build();

        await client.Get("/games/start").AsyncRaw();

        Assert.Equal("/games/result", finalPath);
    }

    [Fact]
    public void Malformed_redirect_location_is_normalized_to_request_failed()
    {
        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            HttpRedirectPolicy.ResolveLocation(new Uri("http://127.0.0.1/start"), "http://["));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, exception.Kind);
    }

    [Fact]
    public async Task Preserves_authorization_on_same_origin_redirect()
    {
        string? authAtResult = null;
        using var server = new TestHttpServer(async ctx =>
        {
            if (ctx.Request.Url!.AbsolutePath == "/start")
            {
                ctx.Response.StatusCode = 307;
                ctx.Response.AddHeader("Location", "/result");
                return;
            }

            authAtResult = ctx.Request.Headers["authorization"];
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl)
            .BearerToken("secret").FollowRedirects(3).Build();

        await client.Get("/start").AsyncRaw();

        Assert.Equal("Bearer secret", authAtResult);
    }

    [Fact]
    public async Task Strips_authorization_on_cross_origin_redirect()
    {
        string? authAtOther = null;
        using var other = new TestHttpServer(async ctx =>
        {
            authAtOther = ctx.Request.Headers["authorization"];
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var origin = new TestHttpServer(async ctx =>
        {
            ctx.Response.StatusCode = 307;
            ctx.Response.AddHeader("Location", $"{other.BaseUrl}/result");
            await Task.CompletedTask;
        });
        using var client = ZLinkHttpClient.Create(origin.BaseUrl)
            .BearerToken("secret").FollowRedirects(3).Build();

        await client.Get("/start").AsyncRaw();

        Assert.Null(authAtOther);
    }

    [Fact]
    public async Task Does_not_restore_authorization_after_cross_origin_redirect_returns_to_origin()
    {
        string? authAtOther = null;
        string? authWhenReturned = null;
        TestHttpServer? origin = null;
        using var other = new TestHttpServer(async ctx =>
        {
            authAtOther = ctx.Request.Headers["authorization"];
            ctx.Response.StatusCode = 307;
            ctx.Response.AddHeader("Location", $"{origin!.BaseUrl}/returned");
            await Task.CompletedTask;
        });
        using (origin = new TestHttpServer(async ctx =>
               {
                   if (ctx.Request.Url!.AbsolutePath == "/start")
                   {
                       ctx.Response.StatusCode = 307;
                       ctx.Response.AddHeader("Location", $"{other.BaseUrl}/hop");
                       return;
                   }

                   authWhenReturned = ctx.Request.Headers["authorization"];
                   await ctx.Response.WriteAsync(200, "{}");
               }))
        using (var client = ZLinkHttpClient.Create(origin.BaseUrl)
                   .BearerToken("secret").FollowRedirects(3).Build())
        {
            await client.Get("/start").AsyncRaw();
        }

        Assert.Null(authAtOther);
        Assert.Null(authWhenReturned);
    }

    [Fact]
    public async Task Redirect_limit_exceeded_throws()
    {
        using var server = new TestHttpServer(async ctx =>
        {
            ctx.Response.StatusCode = 302;
            ctx.Response.AddHeader("Location", "/loop");
            await Task.CompletedTask;
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).FollowRedirects(2).Build();

        var ex = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await client.Get("/loop").AsyncRaw());

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, ex.Kind);
    }

    [Fact]
    public async Task Retries_retriable_transport_failure()
    {
        // First connection is dropped (retriable transport error); the retry succeeds.
        using var server = new FlakyRawServer(1);
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Retry(3).Build();

        var response = await client.Get("/r").AsyncRaw();

        Assert.Equal(200, response.Status);
        Assert.True(server.ConnectionCount >= 2, $"connections={server.ConnectionCount}");
    }

    [Fact]
    public async Task Retry_exhausted_transport_failure_is_unavailable()
    {
        using var server = new FlakyRawServer(10);
        using var client = ZLinkHttpClient.Create(server.BaseUrl)
            .Retry(1)
            .Build();

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await client.Get("/unavailable").AsyncRaw());

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
        Assert.True(
            failure.InnerException is HttpRequestException
                or System.Net.Sockets.SocketException,
            failure.InnerException?.GetType().FullName);
    }

    [Fact]
    public async Task Retry_exhausted_surfaces_timeout()
    {
        // Every attempt times out; the retry loop runs until attempts are exhausted, then throws.
        using var server = new TestHttpServer(async ctx =>
        {
            await Task.Delay(400);
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl)
            .Timeout(TimeSpan.FromMilliseconds(60)).Retry(2).Build();

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await client.Get("/slow").AsyncRaw());
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
        Assert.IsType<TimeoutException>(failure.InnerException);
    }

    [Fact]
    public async Task Cookie_jar_round_trip_with_path_scope()
    {
        string? cookieAtScoped = null;
        string? cookieAtRoot = null;
        using var server = new TestHttpServer(async ctx =>
        {
            switch (ctx.Request.Url!.AbsolutePath)
            {
                case "/login":
                    ctx.Response.AddHeader("Set-Cookie", "session=abc; Path=/secure");
                    await ctx.Response.WriteAsync(200, "{}");
                    break;
                case "/secure/data":
                    cookieAtScoped = ctx.Request.Headers["cookie"];
                    await ctx.Response.WriteAsync(200, "{}");
                    break;
                default:
                    cookieAtRoot = ctx.Request.Headers["cookie"];
                    await ctx.Response.WriteAsync(200, "{}");
                    break;
            }
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Cookies().Build();

        await client.Get("/login").AsyncRaw();
        await client.Get("/secure/data").AsyncRaw();
        await client.Get("/open").AsyncRaw();

        Assert.Equal("session=abc", cookieAtScoped);
        Assert.Null(cookieAtRoot);
    }

    [Theory]
    [InlineData("gzip")]
    [InlineData("deflate")]
    public async Task Transparently_decodes_compressed_response(string encoding)
    {
        var payload = """{"id":42,"name":"Compressed"}""";
        using var server = new TestHttpServer(async ctx =>
        {
            var raw = Encoding.UTF8.GetBytes(payload);
            var compressed = Compress(raw, encoding);
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            ctx.Response.AddHeader("Content-Encoding", encoding);
            ctx.Response.ContentLength64 = compressed.Length;
            await ctx.Response.OutputStream.WriteAsync(compressed);
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Compression().Build();

        var response = await client.Get("/data").Async<Player>();

        Assert.Equal(42, response.Body.Id);
        Assert.False(response.Headers.ContainsKey("Content-Encoding"));
    }

    [Fact]
    public async Task Timeout_throws_retriable_request_failed()
    {
        using var server = new TestHttpServer(async ctx =>
        {
            await Task.Delay(500);
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl)
            .Timeout(TimeSpan.FromMilliseconds(80)).Build();

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await client.Get("/slow").AsyncRaw());
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
        Assert.IsType<TimeoutException>(failure.InnerException);
    }

    [Fact]
    public async Task Max_response_body_size_is_enforced()
    {
        using var server = new TestHttpServer(async ctx =>
            await ctx.Response.WriteAsync(200, new string('x', 4096), "text/plain"));
        using var client = ZLinkHttpClient.Create(server.BaseUrl).MaxResponseBodySize(1024).Build();

        var ex =
            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () => await client.Get("/big").AsyncRaw());

        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, ex.Kind);
    }

    [Fact]
    public async Task Non_blocking_concurrency_does_not_serialize_requests()
    {
        using var server = new TestHttpServer(async ctx =>
        {
            await Task.Delay(200);
            await ctx.Response.WriteAsync(200, "{}");
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Build();

        var stopwatch = Stopwatch.StartNew();
        var tasks = Enumerable.Range(0, 20)
            .Select(_ => client.Get("/r").AsyncRaw().AsTask())
            .ToArray();
        await Task.WhenAll(tasks);
        stopwatch.Stop();

        Assert.All(tasks, t => Assert.Equal(200, t.Result.Status));
        // 20 sequential 200ms requests would take >=4s; concurrent async I/O finishes well under 2s.
        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(2), $"elapsed={stopwatch.Elapsed}");
    }

    [Fact]
    public void Validation_rejects_invalid_configuration()
    {
        Assert.Throws<ZLinkFrameworkException>(() => ZLinkHttpClient.Create().Build());
        Assert.Throws<ZLinkFrameworkException>(() => ZLinkHttpClient.Create("ftp://x"));
        Assert.Throws<ZLinkFrameworkException>(() => ZLinkHttpClient.Create("http://["));
        Assert.Throws<ZLinkFrameworkException>(() => ZLinkHttpClient.Create("http://h").Timeout(TimeSpan.Zero));
        Assert.Throws<ZLinkFrameworkException>(() => ZLinkHttpClient.Create("http://h").Proxy("https://p").Build());
        Assert.Throws<ZLinkFrameworkException>(() => ZLinkHttpClient.Create("http://h").FollowRedirects(0));
        Assert.Throws<ZLinkFrameworkException>(() => ZLinkHttpClient.Create("http://h").Retry(0));
    }

    [Fact]
    public void Codec_extension_ignores_stream_registration_without_http_serializer()
    {
        var builder = ZLinkHttpClient.Create("http://h")
            .Codecs(codecs => codecs.Use(StreamOnlyCodecExtension.Instance));

        Assert.NotNull(builder);
    }

    [Fact]
    public async Task Validation_rejects_bad_request_configuration()
    {
        using var client = ZLinkHttpClient.Create("http://127.0.0.1:1").Build();

        Assert.Throws<ZLinkFrameworkException>(() => client.Get("no-slash"));

        var ex = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await client.Post("/r").Body("a", "text/plain").Form("b", "c").AsyncRaw());
        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, ex.Kind);
    }

    [Fact]
    public async Task Basic_and_bearer_auth_set_authorization_header()
    {
        var seen = new List<string?>();
        using var server = new TestHttpServer(async ctx =>
        {
            seen.Add(ctx.Request.Headers["authorization"]);
            await ctx.Response.WriteAsync(200, "{}");
        });

        using (var basic = ZLinkHttpClient.Create(server.BaseUrl).BasicAuth("aria", "secret").Build())
        {
            await basic.Get("/a").AsyncRaw();
        }

        using (var bearer = ZLinkHttpClient.Create(server.BaseUrl).BearerToken("tok-9").Build())
        {
            await bearer.Get("/b").AsyncRaw();
        }

        Assert.Equal("Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes("aria:secret")), seen[0]);
        Assert.Equal("Bearer tok-9", seen[1]);
    }

    [Fact]
    public async Task Secure_cookie_is_not_sent_over_http_and_max_age_zero_deletes()
    {
        string? cookieAfterSecure = null;
        string? cookieAfterDelete = null;
        using var server = new TestHttpServer(async ctx =>
        {
            switch (ctx.Request.Url!.AbsolutePath)
            {
                case "/set-secure":
                    ctx.Response.AddHeader("Set-Cookie", "sid=abc; Secure");
                    ctx.Response.AddHeader("Set-Cookie", "keep=1");
                    await ctx.Response.WriteAsync(200, "{}");
                    break;
                case "/check-1":
                    cookieAfterSecure = ctx.Request.Headers["cookie"];
                    ctx.Response.AddHeader("Set-Cookie", "keep=; Max-Age=0");
                    await ctx.Response.WriteAsync(200, "{}");
                    break;
                default:
                    cookieAfterDelete = ctx.Request.Headers["cookie"];
                    await ctx.Response.WriteAsync(200, "{}");
                    break;
            }
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Cookies().Build();

        await client.Get("/set-secure").AsyncRaw();
        await client.Get("/check-1").AsyncRaw();
        await client.Get("/check-2").AsyncRaw();

        Assert.Equal("keep=1", cookieAfterSecure); // secure cookie withheld over http
        Assert.Null(cookieAfterDelete); // keep deleted by Max-Age=0
    }

    [Fact]
    public async Task Compressed_malformed_body_throws_payload_decode_failed()
    {
        using var server = new TestHttpServer(async ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.AddHeader("Content-Encoding", "gzip");
            var garbage = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 };
            ctx.Response.ContentLength64 = garbage.Length;
            await ctx.Response.OutputStream.WriteAsync(garbage);
        });
        using var client = ZLinkHttpClient.Create(server.BaseUrl).Compression().Build();

        var ex =
            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () => await client.Get("/bad").AsyncRaw());

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, ex.Kind);
    }

    [Fact]
    public async Task Trusts_test_certificate_over_https()
    {
        using var serverCert = TestCertificates.CreateSelfSigned("zlink-server");
        var (certPath, keyPath) = TestCertificates.WritePem(serverCert);
        try
        {
            using var server = new TlsTestServer(serverCert);
            using var client = ZLinkHttpClient.Create(server.BaseUrl)
                .TrustCertificateFile(certPath)
                .Build();

            var response = await client.Get("/secure").AsyncRaw();

            Assert.Equal(200, response.Status);
        }
        finally
        {
            File.Delete(certPath);
            File.Delete(keyPath);
        }
    }

    [Fact]
    public async Task Untrusted_https_certificate_is_rejected()
    {
        using var serverCert = TestCertificates.CreateSelfSigned("zlink-server");
        using var otherCert = TestCertificates.CreateSelfSigned("zlink-other");
        var (otherCertPath, otherKeyPath) = TestCertificates.WritePem(otherCert);
        try
        {
            using var server = new TlsTestServer(serverCert);
            // Trusts a different certificate than the one the server presents.
            using var client = ZLinkHttpClient.Create(server.BaseUrl)
                .TrustCertificateFile(otherCertPath)
                .Build();

            await Assert.ThrowsAnyAsync<Exception>(async () => await client.Get("/secure").AsyncRaw());
        }
        finally
        {
            File.Delete(otherCertPath);
            File.Delete(otherKeyPath);
        }
    }

    [Fact]
    public async Task Presents_client_certificate_for_mtls()
    {
        using var serverCert = TestCertificates.CreateSelfSigned("zlink-server");
        using var clientCert = TestCertificates.CreateSelfSigned("zlink-client");
        var (serverCertPath, serverKeyPath) = TestCertificates.WritePem(serverCert);
        var (clientCertPath, clientKeyPath) = TestCertificates.WritePem(clientCert);
        try
        {
            using var server = new TlsTestServer(serverCert, true);
            using var client = ZLinkHttpClient.Create(server.BaseUrl)
                .TrustCertificateFile(serverCertPath)
                .ClientCertificateFile(clientCertPath, clientKeyPath)
                .Build();

            var response = await client.Get("/mtls").AsyncRaw();
            var presented = await server.PresentedClientCertificate;

            Assert.Equal(200, response.Status);
            Assert.NotNull(presented);
            Assert.Contains("zlink-client", presented!.Subject);
        }
        finally
        {
            File.Delete(serverCertPath);
            File.Delete(serverKeyPath);
            File.Delete(clientCertPath);
            File.Delete(clientKeyPath);
        }
    }

    [Fact]
    public void Proxy_options_configure_the_handler()
    {
        using var proxied = ZLinkHttpClient.Create("https://api.internal")
            .Proxy("http://proxy.internal:3128")
            .ProxyBasicAuth("pu", "pp")
            .MaxResponseBodySize(2048)
            .Build();

        Assert.NotNull(proxied);
    }

    [Fact]
    public async Task One_shot_shortcut_builds_client_on_demand()
    {
        string? method = null;
        using var server = new TestHttpServer(async ctx =>
        {
            method = ctx.Request.HttpMethod;
            await ctx.Response.WriteAsync(200, """{"id":3,"name":"OneShot"}""");
        });

        var player = await ZLinkHttpClient.Create(server.BaseUrl).Put("/players/3")
            .Body(new Player(3, "OneShot")).Async<Player>();

        Assert.Equal("PUT", method);
        Assert.Equal(3, player.Body.Id);
    }

    [Fact]
    public void Standalone_and_server_request_surfaces_expose_only_valid_terminators()
    {
        Assert.Null(typeof(ZLinkHttpRequestBuilder).GetMethod("Yield"));
        Assert.Null(typeof(ZLinkHttpRequestBuilder).GetMethod("Submit"));
        //  Spec http-client 05 §5.1: the server builder returns the shared Spot
        //  gate with Yield where the framework allows it; the standalone client
        //  has no gate to return.
        Assert.NotNull(typeof(ZLinkHttpServerRequestBuilder).GetMethod("Yield"));
        Assert.Contains(
            typeof(ZLinkHttpServerRequestBuilder).GetMethods(),
            method => method.Name == "Async"
                && !method.IsGenericMethod
                && method.ReturnType == typeof(ValueTask));
        Assert.Null(typeof(ZLinkHttpServerRequestBuilder).GetMethod("Submit"));
    }

    [Fact]
    public async Task Callback_completion_enters_the_captured_execution_turn()
    {
        using var server = new TestHttpServer(async ctx =>
            await ctx.Response.WriteAsync(200, """{"id":11,"name":"Callback"}"""));
        var turn = new CapturedExecutionTurn();
        using var client = ZLinkHttpClient.Create(server.BaseUrl)
            .BuildServer(new FixedExecutionScheduler(turn));
        var completed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Exception? callbackError = null;
        Player? callbackBody = null;

        client.Get("/callback").Async<Player>((error, response) =>
        {
            callbackError = error;
            callbackBody = response?.Body;
            completed.TrySetResult();
        });

        await turn.Posted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(completed.Task.IsCompleted);
        turn.RunPosted();
        await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Null(callbackError);
        Assert.Equal(11, callbackBody?.Id);
        Assert.Empty(turn.Errors);
    }

    private static byte[] Compress(byte[] input, string encoding)
    {
        using var output = new MemoryStream();
        Stream compressor = encoding == "gzip"
            ? new GZipStream(output, CompressionMode.Compress, true)
            : new ZLibStream(output, CompressionMode.Compress, true);
        using (compressor)
        {
            compressor.Write(input, 0, input.Length);
        }

        return output.ToArray();
    }

    private sealed record Player(int Id, string Name);

    private sealed record CreateGameReq(string Name);

    private sealed record CreateGameRes(string Id, bool Ranked);

    private sealed class FixedExecutionScheduler(IZLinkHttpExecutionTurn turn) : IZLinkHttpExecutionScheduler
    {
        public IZLinkHttpExecutionTurn Capture() => turn;
    }

    private sealed class CapturedExecutionTurn : IZLinkHttpExecutionTurn
    {
        private Action? _posted;

        public TaskCompletionSource Posted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public List<Exception> Errors { get; } = [];

        public ValueTask<TResult> YieldAsync<TResult>(
            Func<CancellationToken, ValueTask<TResult>> operation,
            CancellationToken cancellationToken = default)
        {
            return operation(cancellationToken);
        }

        public void Post(Action callback)
        {
            _posted = callback;
            Posted.TrySetResult();
        }

        public void ReportError(Exception exception)
        {
            Errors.Add(exception);
        }

        public void RunPosted()
        {
            Assert.NotNull(_posted);
            _posted();
        }
    }

    private sealed class StreamOnlyCodecExtension :
        IZLinkCodecExtension,
        IZlinkStreamCodecRegistration
    {
        public static StreamOnlyCodecExtension Instance { get; } = new();

        public void Register(IZLinkCodecRegistrar codecs)
        {
        }

        public string ContentType => "application/x-stream-only";

        public ZlinkStreamCodec Codec => ZlinkStreamCodec.Protobuf;
    }

    [MessagePackObject]
    public sealed class PackedPlayer
    {
        [Key(0)] public int Id { get; set; }

        [Key(1)] public string Name { get; set; } = string.Empty;
    }
}
