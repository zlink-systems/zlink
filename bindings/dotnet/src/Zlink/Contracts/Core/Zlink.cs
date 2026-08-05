// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Library entry point: factories for contexts, timers, pollers, and threads,
///     plus process-wide utilities (version, capabilities, proxying).
/// </summary>
public static partial class Zlink
{
    /// <summary>
    ///     Raised when a user callback throws an exception. Callbacks run on a
    ///     background dispatch thread, so such exceptions cannot propagate to the
    ///     caller; subscribe here to observe them.
    /// </summary>
    public static event Action<Exception>? UnhandledCallbackException
    {
        add => CallbackExceptionHub.UnhandledCallbackException += value;
        remove => CallbackExceptionHub.UnhandledCallbackException -= value;
    }

    /// <summary>
    ///     Gets the human-readable message for a native zlink error code.
    /// </summary>
    public static string Strerror(int errnum)
    {
        return StrerrorCore(errnum);
    }

    /// <summary>
    ///     Gets the native zlink version.
    /// </summary>
    public static (int Major, int Minor, int Patch) Version()
    {
        return VersionCore();
    }

    /// <summary>
    ///     Creates a messaging context. The caller owns the returned context and
    ///     must dispose it; disposing it terminates the sockets created from it.
    /// </summary>
    public static IContext CreateContext()
    {
        return new Context();
    }

    /// <summary>
    ///     Creates an atomic counter. The caller owns the returned counter and must
    ///     dispose it.
    /// </summary>
    public static IAtomicCounter CreateAtomicCounter()
    {
        return new AtomicCounter();
    }

    /// <summary>
    ///     Creates a stopwatch. The caller owns the returned stopwatch and must
    ///     dispose it.
    /// </summary>
    public static IZlinkStopwatch CreateStopwatch()
    {
        return new ZlinkStopwatch();
    }

    /// <summary>
    ///     Creates and immediately starts a background thread running
    ///     <paramref name="task" />. The caller owns the returned thread and must
    ///     dispose it (or <see cref="IZlinkThread.Join" /> then
    ///     <see cref="IZlinkThread.Close" />) to release it.
    /// </summary>
    public static IZlinkThread CreateThread(Action task)
    {
        return new ZlinkThread(task);
    }

    /// <summary>
    ///     Creates a poller. The caller owns the returned poller and must dispose
    ///     it.
    /// </summary>
    public static IPoller CreatePoller()
    {
        return new Poller();
    }

    /// <summary>
    ///     Creates a standalone timer. The caller owns the returned timer and must
    ///     dispose it.
    /// </summary>
    public static IZlinkTimer CreateTimer()
    {
        return new Timer();
    }

    /// <summary>
    ///     Reports whether the native library was built with the named capability.
    /// </summary>
    public static bool Has(string capability)
    {
        return HasCore(capability);
    }

    /// <summary>
    ///     Forwards messages between two sockets. This blocks the calling thread
    ///     until the context is terminated, so run it on a dedicated thread.
    /// </summary>
    public static void Proxy(IZlinkSocket frontend, IZlinkSocket backend,
        IZlinkSocket? capture = null)
    {
        ProxyCore(frontend, backend, capture);
    }

    /// <summary>
    ///     Forwards messages between two sockets under runtime control of
    ///     <paramref name="control" />. This blocks the calling thread until the
    ///     proxy is terminated, so run it on a dedicated thread.
    /// </summary>
    public static void ProxySteerable(IZlinkSocket frontend, IZlinkSocket backend,
        IZlinkSocket? capture, IZlinkSocket control)
    {
        ProxySteerableCore(frontend, backend, capture, control);
    }

    /// <summary>
    ///     Sleeps for the specified duration.
    /// </summary>
    public static void Sleep(TimeSpan duration)
    {
        SleepCore(duration);
    }

    /// <summary>
    ///     Disposes each message in a multipart payload.
    /// </summary>
    public static void MultipartClose(IReadOnlyList<Message> parts)
    {
        MultipartCloseCore(parts);
    }
}
