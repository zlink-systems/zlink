using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class LocalJobBacklogTests
{
    [Fact]
    public async Task Local_backlog_warns_once_until_local_waiters_reach_zero()
    {
        var logger = new CaptureLogger();
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 3, 1, 3),
            logger: logger
        );
        var held = new List<ZLinkApplicationJobQueueLease>();
        var waits = new List<(CancellationTokenSource Stop, Task Task)>();
        CancellationTokenSource Add(ZLinkApplicationJobOrigin origin)
        {
            var stop = new CancellationTokenSource();
            var task = queue.AcquireAsync(stop.Token, origin).AsTask();
            waits.Add((stop, task));
            return stop;
        }
        try
        {
            for (var i = 0; i < 3; i++)
                held.Add(await queue.AcquireAsync(default, ZLinkApplicationJobOrigin.Remote));
            for (var i = 0; i < 4; i++)
                Add(ZLinkApplicationJobOrigin.Remote);
            Assert.Empty(logger.Records);
            for (var episode = 1; episode <= 2; episode++)
            {
                var local = Enumerable
                    .Range(0, 3)
                    .Select(_ => Add(ZLinkApplicationJobOrigin.Local))
                    .ToArray();
                Assert.Equal(episode - 1, logger.Records.Count);
                var excess = Add(ZLinkApplicationJobOrigin.Local);
                Assert.Equal(episode, logger.Records.Count);
                queue.ResetMetrics();
                var record = logger.Records[^1];
                Assert.Equal(LogLevel.Warning, record.Level);
                Assert.Equal(4UL, record.Fields["local_waiters"]);
                Assert.Equal(3UL, record.Fields["effective_maximum"]);
                excess.Cancel();
                var repeated = Add(ZLinkApplicationJobOrigin.Local);
                Assert.Equal(episode, logger.Records.Count);
                repeated.Cancel();
                foreach (var stop in local)
                    stop.Cancel();
            }
        }
        finally
        {
            foreach (var (stop, task) in waits)
            {
                stop.Cancel();
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);
                stop.Dispose();
            }
            foreach (var permit in held)
                permit.Dispose();
        }
    }

    private sealed class CaptureLogger : ILogger<ZLinkApplicationJobQueue>
    {
        internal bool Throw { get; init; }
        internal int Attempts { get; private set; }
        internal List<(LogLevel Level, Dictionary<string, object?> Fields)> Records { get; } =
            new();

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter
        )
        {
            Attempts++;
            if (Throw)
                throw new InvalidOperationException("logger failure");
            Assert.Contains(
                "zlink.runtime.host.local_job_backlog_exceeded",
                formatter(state, exception)
            );
            Records.Add(
                (
                    logLevel,
                    ((IEnumerable<KeyValuePair<string, object?>>)(object)state!).ToDictionary()
                )
            );
        }
    }

    [Fact]
    public async Task Logger_failure_preserves_waiting_and_granted_acquisitions()
    {
        var logger = new CaptureLogger { Throw = true };
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1),
            logger: logger
        );
        using var held = await queue.AcquireAsync(default, ZLinkApplicationJobOrigin.Remote);
        var pending = Enumerable
            .Range(0, 3)
            .Select(_ => queue.AcquireAsync(default, ZLinkApplicationJobOrigin.Local).AsTask())
            .ToArray();
        Assert.Equal(1, logger.Attempts);
        Assert.Equal(3UL, queue.GetStatus().CapacityWaiters);
        held.Dispose();
        foreach (var task in pending)
        {
            using var permit = await task;
            Assert.Equal(ZLinkApplicationJobOrigin.Local, permit.Origin);
        }
        Assert.Equal(0UL, queue.GetStatus().CapacityWaiters);
    }

    [Fact]
    public async Task Permit_reacquisition_keeps_the_original_local_origin()
    {
        var logger = new CaptureLogger();
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1),
            logger: logger
        );
        var originals = new List<ZLinkApplicationJobQueueLease>();
        for (var i = 0; i < 2; i++)
        {
            var original = await queue.AcquireAsync(default, ZLinkApplicationJobOrigin.Local);
            original.Dispose();
            originals.Add(original);
        }
        using var held = await queue.AcquireAsync(default, ZLinkApplicationJobOrigin.Remote);
        async Task Reacquire(ZLinkApplicationJobQueueLease original)
        {
            using var scope = ZLinkApplicationJobQueueInvocation.Enter(original);
            await ZLinkApplicationJobQueueInvocation.EnsureQueuedPermitAsync(default);
            Assert.Equal(
                ZLinkApplicationJobOrigin.Local,
                ZLinkApplicationJobQueueInvocation.CurrentOrigin
            );
            await ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStartAsync();
        }
        var pending = originals.Select(Reacquire).ToArray();
        Assert.Single(logger.Records);
        held.Dispose();
        await Task.WhenAll(pending);
        Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
    }
}
