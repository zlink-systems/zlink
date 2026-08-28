// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal static class CallbackDelivery
{
    internal static void Post(SynchronizationContext? context, Action action)
    {
        if (action == null)
            throw new ArgumentNullException(nameof(action));

        if (context != null)
            try
            {
                context.Post(static state => { ExecuteSafely((Action)state!); }, action);
                return;
            }
            catch
            {
                // The captured context no longer accepts work. Core owns the
                // callback thread, so deliver on the current callback context.
            }

        ExecuteSafely(action);
    }

    private static void ExecuteSafely(Action action)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
        }
    }

}
