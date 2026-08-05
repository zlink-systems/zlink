namespace Zlink.Framework.Contracts.Spots;

/// <summary>
///     Actor-free lifecycle for a Spot activated by an Instance Spot intent.
/// </summary>
public interface IZLinkInstanceSpot
{
    IZLinkInstanceSpotContext Context { get; }

    void Configure()
    {
    }

    ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}

/// <summary>
///     Context available to one activated Instance Spot. It intentionally has
///     no Actor membership operations.
/// </summary>
public interface IZLinkInstanceSpotContext : IZLinkSpotCommonContext
{
    IZLinkInstanceSpotHandlerRegistry Handlers { get; }

    ValueTask<bool> CloseAsync(
        CancellationToken cancellationToken = default);
}
