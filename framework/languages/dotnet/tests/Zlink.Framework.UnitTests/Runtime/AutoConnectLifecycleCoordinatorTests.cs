using System.Runtime.CompilerServices;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests;

public sealed class AutoConnectLifecycleCoordinatorTests
{
    [Fact]
    public async Task FrameworkPhaseOwnsExactlyOneGeneration()
    {
        var starts = 0;
        var stops = 0;
        var coordinator = CreateCoordinator(() => starts++, () => stops++);
        var state = UninitializedState();

        await coordinator.FrameworkReadyAsync(state, CancellationToken.None);
        await coordinator.FrameworkReadyAsync(state, CancellationToken.None);
        await coordinator.StopAsync(CancellationToken.None);
        await coordinator.StopAsync(CancellationToken.None);

        Assert.Equal(1, starts);
        Assert.Equal(1, stops);
    }

    private static ZLinkAutoConnectLifecycleCoordinator CreateCoordinator(
        Action started,
        Action stopped)
    {
        return new ZLinkAutoConnectLifecycleCoordinator(
            (_, _) =>
            {
                started();
                return ValueTask.CompletedTask;
            },
            _ =>
            {
                stopped();
                return ValueTask.CompletedTask;
            });
    }

    private static ZLinkFrameworkComponentState UninitializedState() =>
        (ZLinkFrameworkComponentState)RuntimeHelpers.GetUninitializedObject(
            typeof(ZLinkFrameworkComponentState));
}
