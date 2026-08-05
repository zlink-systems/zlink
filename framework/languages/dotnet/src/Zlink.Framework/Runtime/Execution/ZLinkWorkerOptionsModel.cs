namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkWorkerOptionsModel : IZLinkWorkerOptions
{
    public int MinThreads { get; set; }

    public int MaxThreads { get; set; } = Math.Max(2, Environment.ProcessorCount * 2);

    public TimeSpan IdleTimeout { get; set; } = TimeSpan.FromSeconds(30);

    public int MaxQueueLength { get; set; } = 1024;

    public ZLinkWorkerPool CreatePool()
    {
        return new ZLinkWorkerPool(MinThreads, MaxThreads, IdleTimeout, MaxQueueLength);
    }
}