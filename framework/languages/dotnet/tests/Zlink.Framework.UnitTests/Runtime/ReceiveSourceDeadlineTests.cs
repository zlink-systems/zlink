using System.Diagnostics;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

public sealed class ReceiveSourceDeadlineTests
{
    private static readonly TimeSpan Guard = TimeSpan.FromSeconds(5);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task LargeFiniteWaitRemainsPendingUntilWoken(bool additionOverflowOnly)
    {
        var timeout = TimeSpan.MaxValue;
        if (additionOverflowOnly)
        {
            var now = Stopwatch.GetTimestamp();
            var delta = long.MaxValue - now / 2;
            timeout = TimeSpan.FromTicks(
                (long)
                    Int128.Min(
                        TimeSpan.MaxValue.Ticks,
                        (Int128)delta * TimeSpan.TicksPerSecond / Stopwatch.Frequency
                    )
            );
            var representedDelta =
                (Int128)timeout.Ticks * Stopwatch.Frequency / TimeSpan.TicksPerSecond;
            Assert.True(representedDelta <= long.MaxValue);
            Assert.True(now + representedDelta > long.MaxValue);
        }

        await using var source = ZLinkProcessExecutionPool.CreateReceiveSource();
        var waiting = source.WaitAsync([], timeout).AsTask();
        try
        {
            // The registered deadline itself establishes that no elapsed-time race can
            // turn this enormous finite wait into an immediate empty receive.
            Assert.Equal(long.MaxValue, source.Deadline);
            Assert.False(waiting.IsCompleted);
        }
        finally
        {
            source.Wake();
            Assert.Equal(0, await waiting.WaitAsync(Guard));
        }
    }

    [Fact]
    public async Task RepresentableFiniteWaitStillExpiresWithoutWake()
    {
        await using var source = ZLinkProcessExecutionPool.CreateReceiveSource();
        var started = Stopwatch.GetTimestamp();
        var waiting = source.WaitAsync([], TimeSpan.FromMilliseconds(20)).AsTask();
        Assert.InRange(source.Deadline, started + 1, long.MaxValue - 1);
        Assert.Equal(0, await waiting.WaitAsync(Guard));
        Assert.True(Stopwatch.GetTimestamp() >= source.Deadline);
    }

    [Fact]
    public async Task InfiniteWaitRemainsWakeableAndZeroWaitCompletes()
    {
        await using var source = ZLinkProcessExecutionPool.CreateReceiveSource();
        var infinite = source.WaitAsync([], Timeout.InfiniteTimeSpan).AsTask();
        Assert.Equal(long.MaxValue, source.Deadline);
        Assert.False(infinite.IsCompleted);
        source.Wake();
        Assert.Equal(0, await infinite.WaitAsync(Guard));
        Assert.Equal(0, await source.WaitAsync([], TimeSpan.Zero).AsTask().WaitAsync(Guard));
    }
}
