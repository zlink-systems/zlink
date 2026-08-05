namespace Systems.Zlink.Stream.Connector.Runtime;

// Process-global monotonic correlation id for outbound stream packets (hex of a
// per-process counter). Mirrors the C++ client_call_codec generator: unique within a
// process, not globally — nodes are correlated only when the id propagates
// (client request -> server echo).
internal static class ZlinkStreamCorrelation
{
    private static long _counter;

    public static string Next()
    {
        return Convert.ToString(Interlocked.Increment(ref _counter), 16);
    }
}