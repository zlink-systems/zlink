using System.Diagnostics;
using Xunit.Sdk;

namespace Zlink.Framework.UnitTests;

/// <summary>
///     Records when named steps of a timing-sensitive test happen, so a timeout or assertion
///     failure can show which step was slow. Marks are thread-safe; a name marked again is kept
///     in order. Nothing is written unless the test fails and calls <see cref="Rethrow" />.
/// </summary>
internal sealed class FailureTimeline(params string[] expectedSteps)
{
    private readonly object _gate = new();
    private readonly List<(string Name, long Timestamp)> _marks = [];
    private readonly long _start = Stopwatch.GetTimestamp();

    /// <summary>The failures a timing test explains with a timeline.</summary>
    internal static bool IsTimingFailure(Exception error) =>
        error
            is TimeoutException
                or XunitException
                or ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.DeadlineExceeded };

    internal long StartTimestamp => _start;

    internal void Mark(string name)
    {
        var timestamp = Stopwatch.GetTimestamp();
        lock (_gate)
            _marks.Add((name, timestamp));
    }

    /// <summary>
    ///     Lists expected steps in order (<c>unobserved</c> when never marked), then any other
    ///     marked step, then the time of the call as <c>failed</c>.
    /// </summary>
    internal string Format()
    {
        (string Name, long Timestamp)[] marks;
        lock (_gate)
            marks = [.. _marks];
        var parts = new List<string>();
        foreach (var name in expectedSteps)
        {
            var seen = marks.Where(mark => mark.Name == name).ToArray();
            if (seen.Length == 0)
                parts.Add($"{name}=unobserved");
            else
                parts.AddRange(seen.Select(mark => $"{name}={Elapsed(mark.Timestamp)}"));
        }
        parts.AddRange(
            marks
                .Where(mark => !expectedSteps.Contains(mark.Name))
                .Select(mark => $"{mark.Name}={Elapsed(mark.Timestamp)}")
        );
        parts.Add($"failed={Elapsed(Stopwatch.GetTimestamp())}");
        return string.Join(", ", parts) + ".";
    }

    /// <summary>
    ///     Returns the exception to throw from a catch block: the original kind with the
    ///     timeline appended to its message (a framework deadline error becomes a
    ///     <see cref="TimeoutException" />).
    /// </summary>
    internal Exception Rethrow(Exception error, string? detail = null)
    {
        var message =
            $"{error.Message}\nTimeline (ms since observation started{detail}) {Format()}";
        return error is XunitException
            ? new XunitException(message, error)
            : new TimeoutException(message, error);
    }

    private string Elapsed(long timestamp) =>
        FormattableString.Invariant(
            $"{Stopwatch.GetElapsedTime(_start, timestamp).TotalMilliseconds:F3}"
        );
}
