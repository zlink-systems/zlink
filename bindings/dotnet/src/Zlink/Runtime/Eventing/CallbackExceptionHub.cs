// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal static class CallbackExceptionHub
{
    public static event Action<Exception>? UnhandledCallbackException;

    internal static void Report(Exception exception)
    {
        try
        {
            UnhandledCallbackException?.Invoke(exception);
        }
        catch
        {
        }
    }
}