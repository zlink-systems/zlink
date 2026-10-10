using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.ContractTests.Errors;

public sealed class ErrorContracts
{
    [Theory]
    [InlineData(false, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData(true, ZLinkFrameworkErrorKind.Unavailable)]
    public async Task Tokenless_submit_rejection_is_unavailable_in_both_phases(
        bool completionFailure,
        ZLinkFrameworkErrorKind expected
    )
    {
        var failure = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.Backpressured);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkRawRequestSubmitter.SubmitAsync(
                Array.Empty<Message>(),
                (_, _, _) =>
                    completionFailure
                        ? Task.FromException<IReadOnlyList<Message>>(failure)
                        : throw failure,
                TimeSpan.FromSeconds(1),
                "request failed: {0}",
                CancellationToken.None
            )
        );

        Assert.Equal(expected, error.Kind);
        Assert.IsType<ZlinkSubmitException>(error.InnerException);
        Assert.Contains("submission capacity is unavailable", error.Message);
        Assert.DoesNotContain("route is not connected", error.Message);
        Assert.DoesNotContain("Backpressured", Enum.GetNames<ZLinkFrameworkErrorKind>());
    }

    [Theory]
    [InlineData(false, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData(true, ZLinkFrameworkErrorKind.Unavailable)]
    public async Task Durable_request_preserves_admission_terminal_without_replaying(
        bool completionFailure,
        ZLinkFrameworkErrorKind expected
    )
    {
        var failure = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.Backpressured);
        var attempts = 0;
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkDurableRequest.RequestAsync(
                [],
                System.Diagnostics.Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(1),
                async (_, remaining, token) =>
                {
                    attempts++;
                    return await ZLinkRawRequestSubmitter.SubmitAsync(
                        Array.Empty<Message>(),
                        (_, _, _) =>
                            completionFailure
                                ? Task.FromException<IReadOnlyList<Message>>(failure)
                                : throw failure,
                        remaining,
                        "request failed: {0}",
                        token
                    );
                },
                CancellationToken.None
            )
        );

        Assert.Equal(expected, error.Kind);
        Assert.Contains("submission capacity is unavailable", error.Message);
        Assert.Equal(1, attempts);
    }

    [Fact]
    public void Framework_exception_contract_matches_the_frozen_surface()
    {
        Assert.Equal(
            new Dictionary<string, int>(StringComparer.Ordinal)
            {
                [nameof(ZLinkFrameworkErrorKind.NotFound)] = 0,
                [nameof(ZLinkFrameworkErrorKind.AlreadyExists)] = 1,
                [nameof(ZLinkFrameworkErrorKind.TypeMismatch)] = 2,
                [nameof(ZLinkFrameworkErrorKind.NotConfigured)] = 3,
                [nameof(ZLinkFrameworkErrorKind.Rejected)] = 4,
                [nameof(ZLinkFrameworkErrorKind.Unavailable)] = 5,
                [nameof(ZLinkFrameworkErrorKind.DeadlineExceeded)] = 6,
                [nameof(ZLinkFrameworkErrorKind.ShuttingDown)] = 7,
                [nameof(ZLinkFrameworkErrorKind.ProtocolError)] = 8,
                [nameof(ZLinkFrameworkErrorKind.InvalidOperation)] = 9,
                [nameof(ZLinkFrameworkErrorKind.DataLost)] = 10,
                [nameof(ZLinkFrameworkErrorKind.InternalFailure)] = 11,
            },
            Enum.GetValues<ZLinkFrameworkErrorKind>()
                .ToDictionary(
                    static value => value.ToString(),
                    static value => (int)value,
                    StringComparer.Ordinal
                )
        );

        Assert.Empty(typeof(ZLinkFrameworkException).GetConstructors());
        Assert.Equal(
            typeof(ZLinkFrameworkErrorKind),
            typeof(ZLinkFrameworkException)
                .GetProperty(nameof(ZLinkFrameworkException.Kind))!
                .PropertyType
        );
        Assert.Null(typeof(ZLinkFrameworkException).GetProperty("RetryAdvice"));
    }

    [Fact]
    public void Default_retry_advice_matches_the_spec_table_for_every_kind()
    {
        var expected = new Dictionary<ZLinkFrameworkErrorKind, ZLinkRetryAdvice>
        {
            [ZLinkFrameworkErrorKind.NotFound] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.AlreadyExists] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.TypeMismatch] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.NotConfigured] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.Rejected] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.Unavailable] = ZLinkRetryAdvice.RetryAfterBackoff,
            [ZLinkFrameworkErrorKind.DeadlineExceeded] = ZLinkRetryAdvice.RetryAfterBackoff,
            [ZLinkFrameworkErrorKind.ShuttingDown] = ZLinkRetryAdvice.RetryAfterStateChange,
            [ZLinkFrameworkErrorKind.ProtocolError] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.InvalidOperation] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.DataLost] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.InternalFailure] = ZLinkRetryAdvice.DoNotRetry,
        };

        var actual = Enum.GetValues<ZLinkFrameworkErrorKind>()
            .ToDictionary(
                static kind => kind,
                static kind => new ZLinkFrameworkException(kind, "kind").RetryAdvice
            );

        Assert.Equal(expected, actual);
    }
}
