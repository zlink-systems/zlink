namespace Zlink.Framework.Runtime.Execution;

internal sealed record ZLinkExecutionLanePolicy
{
    internal static ZLinkExecutionLanePolicy Default { get; } = new(
        applicationMessageCapacity: 1_024,
        applicationByteCapacity: 64L * 1024 * 1024,
        lifecycleMessageCapacity: 128,
        lifecycleByteCapacity: 4L * 1024 * 1024,
        fixedWorkByteCost: 256,
        lifecycleBurstLimit: 8,
        ownerTimeBudget: TimeSpan.FromMilliseconds(10));

    internal ZLinkExecutionLanePolicy(
        int applicationMessageCapacity,
        long applicationByteCapacity,
        int lifecycleMessageCapacity,
        long lifecycleByteCapacity,
        long fixedWorkByteCost,
        int lifecycleBurstLimit,
        TimeSpan ownerTimeBudget)
    {
        if (applicationMessageCapacity <= 0)
            throw new ArgumentOutOfRangeException(
                nameof(applicationMessageCapacity));
        if (applicationByteCapacity <= 0)
            throw new ArgumentOutOfRangeException(
                nameof(applicationByteCapacity));
        if (lifecycleMessageCapacity <= 0)
            throw new ArgumentOutOfRangeException(
                nameof(lifecycleMessageCapacity));
        if (lifecycleByteCapacity <= 0)
            throw new ArgumentOutOfRangeException(
                nameof(lifecycleByteCapacity));
        if (fixedWorkByteCost < 0)
            throw new ArgumentOutOfRangeException(nameof(fixedWorkByteCost));
        if (lifecycleBurstLimit <= 0)
            throw new ArgumentOutOfRangeException(nameof(lifecycleBurstLimit));
        if (ownerTimeBudget <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(ownerTimeBudget));

        ApplicationMessageCapacity = applicationMessageCapacity;
        ApplicationByteCapacity = applicationByteCapacity;
        LifecycleMessageCapacity = lifecycleMessageCapacity;
        LifecycleByteCapacity = lifecycleByteCapacity;
        FixedWorkByteCost = fixedWorkByteCost;
        LifecycleBurstLimit = lifecycleBurstLimit;
        OwnerTimeBudget = ownerTimeBudget;
    }

    internal int ApplicationMessageCapacity { get; }

    internal long ApplicationByteCapacity { get; }

    internal int LifecycleMessageCapacity { get; }

    internal long LifecycleByteCapacity { get; }

    internal long FixedWorkByteCost { get; }

    internal int LifecycleBurstLimit { get; }

    internal TimeSpan OwnerTimeBudget { get; }
}
