namespace Zlink.Framework.Contracts.Workers;

public interface IZLinkWorkerCall<TResult>
{
    IZLinkWorkerCall<TResult> Timeout(TimeSpan timeout);

    void Submit(CancellationToken cancellationToken = default);

    ValueTask<TResult> Async(CancellationToken cancellationToken = default);

    ValueTask<TResult> Yield(CancellationToken cancellationToken = default);
}

public interface IZLinkWorkerOptions
{
    int MinThreads { get; set; }

    int MaxThreads { get; set; }

    TimeSpan IdleTimeout { get; set; }

    int MaxQueueLength { get; set; }
}
