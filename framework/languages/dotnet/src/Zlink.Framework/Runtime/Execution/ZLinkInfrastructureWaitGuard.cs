using System.Diagnostics;

namespace Zlink.Framework.Runtime.Execution;

internal static class ZLinkInfrastructureWaitGuard
{
#if DEBUG
    internal static bool IsInfrastructureContext =>
        ZLinkStateLane.Current is not null || ZLinkSerialTurn.Current?.LifecycleOwner is not null;
#endif

    [Conditional("DEBUG")]
    internal static void ThrowIfBlocking(bool completed, string operation)
    {
#if DEBUG
        if (!completed && IsInfrastructureContext)
            throw new InvalidOperationException(
                $"Infrastructure execution cannot synchronously wait for {operation}."
                    + Environment.NewLine
                    + Environment.StackTrace
            );
#endif
    }
}
