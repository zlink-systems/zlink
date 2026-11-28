// SPDX-License-Identifier: MPL-2.0

internal static class ZeroRateCheck
{
    // A zero target rate schedules nothing, so an idle subscriber must not
    // look caught up after one window (rule 5, same as C, Rust, Go and Python).
    internal static void Run()
    {
        const ulong start = 1_000_000_000;
        var tracker = new PerfMultiPubSubClient.CatchUpTracker(1);
        tracker.Observe(start, [0], 0, 0.0);
        if (tracker.Observe(start + 500_000_000, [0], 0, 0.0))
            throw new InvalidOperationException("caught up without a positive target rate");
        Console.WriteLine("PASS 5 zero target rate cannot catch up");
    }
}
