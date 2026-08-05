using System.Diagnostics;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamHeartbeatMonitor(ZlinkStreamHeartbeatOptions options)
{
    private long _lastInboundTicks;
    private long _lastPingTicks;

    public void RecordSessionStart()
    {
        var now = Stopwatch.GetTimestamp();
        Interlocked.Exchange(ref _lastInboundTicks, now);
        Interlocked.Exchange(ref _lastPingTicks, now);
    }

    public void RecordInbound()
    {
        Interlocked.Exchange(ref _lastInboundTicks, Stopwatch.GetTimestamp());
    }

    public async Task RunAsync(
        Func<CancellationToken, ValueTask>? sendHeartbeatPing,
        Func<ZlinkStreamError, CancellationToken, ValueTask> handleTransportErrorAsync,
        CancellationToken cancellationToken)
    {
        if (!options.Enabled || sendHeartbeatPing is null) return;

        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                await Task.Delay(options.Interval, cancellationToken).ConfigureAwait(false);

                var now = Stopwatch.GetTimestamp();
                if (Elapsed(now, Interlocked.Read(ref _lastInboundTicks)) >= options.Timeout)
                {
                    await handleTransportErrorAsync(
                        new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "Heartbeat timed out."),
                        CancellationToken.None).ConfigureAwait(false);
                    return;
                }

                if (Elapsed(now, Interlocked.Read(ref _lastPingTicks)) >= options.Interval)
                {
                    await sendHeartbeatPing(cancellationToken).ConfigureAwait(false);
                    Interlocked.Exchange(ref _lastPingTicks, Stopwatch.GetTimestamp());
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            var error = ex is ZlinkStreamException streamException
                ? streamException.Error
                : new ZlinkStreamError(ZlinkStreamErrorCode.SendFailed, "Heartbeat send failed.", ex);
            await handleTransportErrorAsync(error, CancellationToken.None).ConfigureAwait(false);
        }
    }

    private static TimeSpan Elapsed(long nowTicks, long previousTicks)
    {
        return TimeSpan.FromSeconds((double)(nowTicks - previousTicks) / Stopwatch.Frequency);
    }
}