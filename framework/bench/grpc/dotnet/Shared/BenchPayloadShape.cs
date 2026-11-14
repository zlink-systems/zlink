namespace WithGrpcBench.Shared;

public static class BenchPayloadShapes
{
    public static BenchPayloadShape Parse(string? value) => value switch
    {
        null or "bytes" => BenchPayloadShape.Bytes,
        "mixed" => BenchPayloadShape.Mixed,
        _ => throw new ArgumentOutOfRangeException(nameof(value), value, "ZLINK_DOTNET_LADDER_SHAPE must be bytes or mixed."),
    };
}

public static class BenchLadderConfig
{
    public static readonly string Level = Environment.GetEnvironmentVariable("ZLINK_DOTNET_LADDER_LEVEL") ?? "L0";
    public static readonly BenchPayloadShape Shape = BenchPayloadShapes.Parse(
        Environment.GetEnvironmentVariable("ZLINK_DOTNET_LADDER_SHAPE")
    );
}
