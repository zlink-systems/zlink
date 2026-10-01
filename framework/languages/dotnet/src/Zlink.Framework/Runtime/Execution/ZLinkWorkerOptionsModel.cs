namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkWorkerOptionsModel : IZLinkWorkerOptions
{
    private const int MinimumDefaultMaxThreads = 2;
    private const int ProcessorThreadMultiplier = 2;
    private static readonly TimeSpan DefaultIdleTimeout = TimeSpan.FromSeconds(30);

    public int MinThreads { get; set; }

    public int MaxThreads { get; set; } =
        Math.Max(MinimumDefaultMaxThreads, Environment.ProcessorCount * ProcessorThreadMultiplier);

    public TimeSpan IdleTimeout { get; set; } = DefaultIdleTimeout;

    public ZLinkWorkerPool CreatePool()
    {
        return new ZLinkWorkerPool(MinThreads, MaxThreads, IdleTimeout);
    }
}
