using System;

// Wire-level stop token used by sender threads / clients to signal phase
// end to a receiver waiting on a poller. PERF_SINGLE_TEST_POLICY § 1.4
// and PERF_MULTI_TEST_POLICY § 1.3.1 mandate this pattern instead of a
// shared `volatile bool senderDone` + short polling.
//
// Wire bytes are identical across all bindings so heterogeneous
// interop scenarios (e.g. cpp sender, dotnet receiver) share the same
// idiom.
public static class StopToken
{
    public const string Literal = "__zlink_perf_stop__";

    public static readonly byte[] Bytes =
        System.Text.Encoding.ASCII.GetBytes(Literal);

    public static bool IsStopToken(ReadOnlySpan<byte> payload)
    {
        return payload.Length == Bytes.Length
            && payload.SequenceEqual(Bytes);
    }
}
