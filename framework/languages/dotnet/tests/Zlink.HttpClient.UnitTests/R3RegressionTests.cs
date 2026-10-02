using System.Net;
using Xunit;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient.Runtime;

namespace Zlink.HttpClient.UnitTests;

public sealed class R3RegressionTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task OneShotRequestFailurePrecedesCloseFailure(bool synchronous)
    {
        var requestFailure = new IOException("request failed");
        var closeFailure = new InvalidOperationException("close failed");
        var closeCount = 0;
        var observed = await Assert.ThrowsAsync<IOException>(async () =>
            await ZLinkHttpRequestBuilder.ExecuteOneShotAsync<int>(
                synchronous
                    ? CaptureSynchronousFailureAsync()
                    : ValueTask.FromException<int>(requestFailure),
                () =>
                {
                    closeCount++;
                    throw closeFailure;
                }
            )
        );
        Assert.Same(requestFailure, observed);
        Assert.Equal(1, closeCount);

        async ValueTask<int> CaptureSynchronousFailureAsync()
        {
            await ValueTask.CompletedTask;
            throw requestFailure;
        }
    }

    [Fact]
    public async Task OneShotSuccessReportsCloseFailure()
    {
        var closeFailure = new InvalidOperationException("close failed");
        var closeCount = 0;
        var observed = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await ZLinkHttpRequestBuilder.ExecuteOneShotAsync(
                ValueTask.FromResult(1),
                () =>
                {
                    closeCount++;
                    throw closeFailure;
                }
            )
        );
        Assert.Same(closeFailure, observed);
        Assert.Equal(1, closeCount);
    }

    [Fact]
    public async Task GeneralExecutionFailureIsInternalFailure()
    {
        var failure = new InvalidOperationException("execution failure");
        var policy = new RetryPolicy(Options());
        var request = new HttpRequestSpec
        {
            Method = ZLinkHttpMethod.Get,
            Target = "/",
            Headers = new Dictionary<string, string>(),
        };
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await policy.ExecuteAsync(
                request,
                (_, _) => ValueTask.FromException<RawHttpResponse>(failure),
                CancellationToken.None
            )
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, observed.Kind);
        Assert.Same(failure, observed.InnerException);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task BodyProviderFailureIsInternalFailure(bool canceled)
    {
        var options = Options();
        using var client = new System.Net.Http.HttpClient(new ResponseHandler());
        var performer = new RequestPerformer(options, new CookieJar(), client);
        Exception failure = canceled
            ? new OperationCanceledException("provider canceled")
            : new IOException("provider failure");
        var request = new HttpRequestSpec
        {
            Method = ZLinkHttpMethod.Post,
            Target = "/",
            Headers = new Dictionary<string, string>(),
            BodyProvider = () => throw failure,
        };
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await performer.PerformAsync(request, CancellationToken.None)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, observed.Kind);
        Assert.Same(failure, observed.InnerException);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DownloadSinkFailureIsInternalFailure(bool canceled)
    {
        var options = Options();
        using var client = new System.Net.Http.HttpClient(new ResponseHandler());
        var performer = new RequestPerformer(options, new CookieJar(), client);
        Exception failure = canceled
            ? new OperationCanceledException("sink canceled")
            : new IOException("sink failure");
        var request = new HttpRequestSpec
        {
            Method = ZLinkHttpMethod.Get,
            Target = "/",
            Headers = new Dictionary<string, string>(),
            Sink = _ => throw failure,
        };
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await performer.PerformAsync(request, CancellationToken.None)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, observed.Kind);
        Assert.Same(failure, observed.InnerException);
    }

    private static HttpClientOptions Options() =>
        new()
        {
            BaseUrl = "http://localhost",
            Headers = new Dictionary<string, string>(),
            Codecs = new HttpClientCodecRegistry(),
        };

    [Fact]
    public async Task CallerCancellationPreservesIdentityWithoutRetry()
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var failure = new OperationCanceledException(cancellation.Token);
        var policy = new RetryPolicy(
            new HttpClientOptions
            {
                BaseUrl = "http://localhost",
                Headers = new Dictionary<string, string>(),
                Codecs = new HttpClientCodecRegistry(),
                RetryAttempts = 2,
            }
        );
        var attempts = 0;
        var request = new HttpRequestSpec
        {
            Method = ZLinkHttpMethod.Get,
            Target = "/",
            Headers = new Dictionary<string, string>(),
        };
        var observed = await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await policy.ExecuteAsync(
                request,
                (_, _) =>
                {
                    attempts++;
                    return ValueTask.FromException<RawHttpResponse>(failure);
                },
                cancellation.Token
            )
        );
        Assert.Same(failure, observed);
        Assert.Equal(1, attempts);
    }

    [Fact]
    public async Task NativeTransportWrapperPreservesClassifiedApplicationFailure()
    {
        var failure = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkHttpClient.Create().Timeout(TimeSpan.Zero)
        );
        using var client = new System.Net.Http.HttpClient(
            new ResponseHandler(new HttpRequestException("native send failed", failure))
        );
        var performer = new RequestPerformer(Options(), new CookieJar(), client);
        var request = new HttpRequestSpec
        {
            Method = ZLinkHttpMethod.Get,
            Target = "/",
            Headers = new Dictionary<string, string>(),
        };
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await performer.PerformAsync(request, CancellationToken.None)
        );
        Assert.Same(failure, observed);
        Assert.Same(failure.InnerException, observed.InnerException);
    }

    [Fact]
    public async Task ApplicationWrapperDoesNotPromoteItsClassifiedCause()
    {
        var coded = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkHttpClient.Create().Timeout(TimeSpan.Zero)
        );
        var failure = new IOException("application wrapper", coded);
        using var client = new System.Net.Http.HttpClient(new ResponseHandler());
        var performer = new RequestPerformer(Options(), new CookieJar(), client);
        var request = new HttpRequestSpec
        {
            Method = ZLinkHttpMethod.Post,
            Target = "/",
            Headers = new Dictionary<string, string>(),
            BodyProvider = () => throw failure,
        };
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await performer.PerformAsync(request, CancellationToken.None)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, observed.Kind);
        Assert.Same(failure, observed.InnerException);
        Assert.Same(coded, observed.InnerException!.InnerException);
    }

    private sealed class ResponseHandler(Exception? transportFailure = null) : HttpMessageHandler
    {
        protected override async Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken
        )
        {
            if (transportFailure is not null)
                throw transportFailure;
            if (request.Content is not null)
                await request.Content.CopyToAsync(Stream.Null, cancellationToken);
            return new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new ByteArrayContent([1]),
            };
        }
    }
}
