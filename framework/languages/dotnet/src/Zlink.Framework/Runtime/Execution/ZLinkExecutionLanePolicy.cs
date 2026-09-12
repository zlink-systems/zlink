namespace Zlink.Framework.Runtime.Execution;

internal sealed record ZLinkExecutionLanePolicy
{
    internal static ZLinkExecutionLanePolicy Default { get; } = new(
        lifecycleBurstLimit: 8,
        ownerTimeBudget: TimeSpan.FromMilliseconds(10));

    internal ZLinkExecutionLanePolicy(
        int lifecycleBurstLimit,
        TimeSpan ownerTimeBudget)
    {
        if (lifecycleBurstLimit <= 0)
            throw new ArgumentOutOfRangeException(nameof(lifecycleBurstLimit));
        if (ownerTimeBudget <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(ownerTimeBudget));

        LifecycleBurstLimit = lifecycleBurstLimit;
        OwnerTimeBudget = ownerTimeBudget;
    }

    internal int LifecycleBurstLimit { get; }

    internal TimeSpan OwnerTimeBudget { get; }
}
