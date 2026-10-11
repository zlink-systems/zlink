namespace WithGrpcBench.Shared;

public static class BenchDiagnostics
{
    public const string FlowTraceVariable = "BENCH_FLOW_TRACE";

    public static bool FlowTraceEnabled =>
        Environment.GetEnvironmentVariable(FlowTraceVariable) is not null;
}
