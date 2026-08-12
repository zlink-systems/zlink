using Zlink.Framework.Contracts.Errors;

namespace Zlink.Framework.ContractTests.Errors;

public sealed class ErrorContracts
{
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
                [nameof(ZLinkFrameworkErrorKind.CapacityExceeded)] = 6,
                [nameof(ZLinkFrameworkErrorKind.DeadlineExceeded)] = 7,
                [nameof(ZLinkFrameworkErrorKind.ShuttingDown)] = 8,
                [nameof(ZLinkFrameworkErrorKind.ProtocolError)] = 9,
                [nameof(ZLinkFrameworkErrorKind.InvalidOperation)] = 10,
                [nameof(ZLinkFrameworkErrorKind.DataLost)] = 11,
                [nameof(ZLinkFrameworkErrorKind.InternalFailure)] = 12
            },
            Enum.GetValues<ZLinkFrameworkErrorKind>()
                .ToDictionary(static value => value.ToString(), static value => (int)value, StringComparer.Ordinal));

        Assert.Empty(typeof(ZLinkFrameworkException).GetConstructors());
        Assert.Equal(typeof(ZLinkFrameworkErrorKind),
            typeof(ZLinkFrameworkException).GetProperty(nameof(ZLinkFrameworkException.Kind))!.PropertyType);
        Assert.Null(typeof(ZLinkFrameworkException).GetProperty(
            "RetryAdvice"));
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
            [ZLinkFrameworkErrorKind.CapacityExceeded] = ZLinkRetryAdvice.RetryAfterBackoff,
            [ZLinkFrameworkErrorKind.DeadlineExceeded] = ZLinkRetryAdvice.RetryAfterBackoff,
            [ZLinkFrameworkErrorKind.ShuttingDown] = ZLinkRetryAdvice.RetryAfterStateChange,
            [ZLinkFrameworkErrorKind.ProtocolError] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.InvalidOperation] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.DataLost] = ZLinkRetryAdvice.DoNotRetry,
            [ZLinkFrameworkErrorKind.InternalFailure] = ZLinkRetryAdvice.DoNotRetry
        };

        var actual = Enum.GetValues<ZLinkFrameworkErrorKind>()
            .ToDictionary(
                static kind => kind,
                static kind => new ZLinkFrameworkException(kind, "kind").RetryAdvice);

        Assert.Equal(expected, actual);
    }
}
