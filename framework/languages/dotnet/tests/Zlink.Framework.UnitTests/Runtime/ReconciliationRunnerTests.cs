namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ReconciliationRunnerTests
{
    [Fact]
    public async Task Retryable_Failures_Are_Reported_Once_And_Then_Succeed()
    {
        var first = new InvalidOperationException("first");
        var second = new IOException("second");
        var reported = new List<Exception>();
        var attempts = 0;

        var result = await ZLinkReconciliationRunner.RunAsync(
            _ =>
            {
                attempts++;
                return attempts switch
                {
                    1 => ValueTask.FromException<int>(first),
                    2 => ValueTask.FromException<int>(second),
                    _ => ValueTask.FromResult(42)
                };
            },
            reported.Add,
            CancellationToken.None,
            retryDelay: TimeSpan.Zero);

        Assert.Equal(42, result);
        Assert.Equal(3, attempts);
        Assert.Equal(new Exception[] { first, second }, reported);
    }

    [Fact]
    public async Task Terminal_Failure_Is_Not_Reported_Or_Retried()
    {
        var terminal = new FormatException("terminal");
        var reported = new List<Exception>();
        var attempts = 0;

        var actual = await Assert.ThrowsAsync<FormatException>(async () =>
            await ZLinkReconciliationRunner.RunAsync(
                _ =>
                {
                    attempts++;
                    return ValueTask.FromException(terminal);
                },
                reported.Add,
                CancellationToken.None,
                static exception => exception is FormatException,
                TimeSpan.Zero));

        Assert.Same(terminal, actual);
        Assert.Equal(1, attempts);
        Assert.Empty(reported);
    }

    [Fact]
    public async Task Shutdown_Stops_After_The_Current_Failure()
    {
        using var shutdown = new CancellationTokenSource();
        var failure = new IOException("retry");
        var reported = new List<Exception>();
        var attempts = 0;

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await ZLinkReconciliationRunner.RunAsync(
                _ =>
                {
                    attempts++;
                    return ValueTask.FromException(failure);
                },
                exception =>
                {
                    reported.Add(exception);
                    shutdown.Cancel();
                },
                shutdown.Token));

        Assert.Equal(1, attempts);
        Assert.Equal(new[] { failure }, reported);
    }

    [Fact]
    public async Task Cancellation_From_The_Operation_Is_Retried_When_It_Is_Not_Shutdown()
    {
        var transientCancellation = new OperationCanceledException("attempt timeout");
        var reported = new List<Exception>();
        var attempts = 0;

        await ZLinkReconciliationRunner.RunAsync(
            _ =>
            {
                attempts++;
                return attempts == 1
                    ? ValueTask.FromException(transientCancellation)
                    : ValueTask.CompletedTask;
            },
            reported.Add,
            CancellationToken.None,
            retryDelay: TimeSpan.Zero);

        Assert.Equal(2, attempts);
        Assert.Equal(new[] { transientCancellation }, reported);
    }
}
