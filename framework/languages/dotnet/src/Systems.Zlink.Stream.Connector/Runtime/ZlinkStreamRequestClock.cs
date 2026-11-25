using System.Diagnostics;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamRequestClock(
    Func<TimeSpan> now,
    Func<TimeSpan, CancellationToken, Task> delay,
    Func<int> jitter
)
{
    internal static ZlinkStreamRequestClock System { get; } =
        new(
            () => TimeSpan.FromSeconds(Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency),
            static (duration, token) =>
                Task.Delay(
                    TimeSpan.FromMilliseconds(Math.Ceiling(duration.TotalMilliseconds)),
                    token
                ),
            static () => global::System.Security.Cryptography.RandomNumberGenerator.GetInt32(51)
        );

    internal TimeSpan Now => now();

    internal Task DelayAsync(TimeSpan duration, CancellationToken token) => delay(duration, token);

    internal int NextJitterMs() => jitter();
}
