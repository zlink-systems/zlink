using System.Diagnostics;

namespace Zlink.Framework.UnitTests;

public sealed class DurableRequestTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task TypedNotConnectedReplaysIdenticalWireWithWholeRemainingDeadline(
        bool wasAdmitted)
    {
        IReadOnlyList<ReadOnlyMemory<byte>> wire = [new byte[] { 28, 1, 47, 48, 49 }];
        var timeout = TimeSpan.FromSeconds(8);
        var started = Stopwatch.GetTimestamp();
        var attempts = 0;
        var firstTimeout = TimeSpan.Zero;
        IReadOnlyList<Message> terminal = [Message.From("terminal"u8)];
        var result = await ZLinkDurableRequest.RequestAsync(wire, started, timeout,
            (frames, remaining, _) =>
            {
                Assert.Same(wire, frames);
                Assert.True(remaining <= timeout - Stopwatch.GetElapsedTime(started)
                    + TimeSpan.FromMilliseconds(10));
                if (++attempts == 1)
                {
                    firstTimeout = remaining;
                    Assert.True(remaining > TimeSpan.FromSeconds(5));
                    return ValueTask.FromException<IReadOnlyList<Message>>(
                        wasAdmitted
                            ? new ZlinkRequestException(ZlinkRequestException.ErrorCode.NotConnected)
                            : new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected));
                }
                Assert.True(remaining < firstTimeout);
                return ValueTask.FromResult(terminal);
            }, CancellationToken.None);
        Assert.Same(terminal, result);
        ZLinkMessageParts.DisposeAll(result);
        Assert.Equal(2, attempts);
    }

    [Fact]
    public async Task AdmittedHistorySurvivesLaterSubmitFailures()
    {
        var attempts = 0;
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkDurableRequest.RequestAsync([], Stopwatch.GetTimestamp(),
                TimeSpan.FromMilliseconds(100), (_, _, _) =>
                    ValueTask.FromException<IReadOnlyList<Message>>(++attempts == 1
                        ? new ZlinkRequestException(ZlinkRequestException.ErrorCode.NotConnected)
                        : new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected)),
                CancellationToken.None));
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        Assert.True(attempts > 1);
    }

    [Fact]
    public async Task CancellationAndUntypedErrorsDoNotReplay()
    {
        var failure = new InvalidOperationException("invalid durable submission");
        var attempts = 0;
        var observed = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await ZLinkDurableRequest.RequestAsync([], Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(1), (_, _, _) =>
                {
                    attempts++;
                    return ValueTask.FromException<IReadOnlyList<Message>>(failure);
                }, CancellationToken.None));
        Assert.Same(failure, observed);
        Assert.Equal(1, attempts);
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await ZLinkDurableRequest.RequestAsync([], Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(1), (_, _, _) =>
                    throw new Xunit.Sdk.XunitException("Cancelled operation submitted."),
                cancellation.Token));
    }
}
