// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     A messaging context: the factory and owner of sockets.
/// </summary>
/// <remarks>
///     Every socket created here is owned by the caller and must be disposed.
///     Disposing the context terminates anything still open under it.
/// </remarks>
public interface IContext : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Gets the context-wide options facade.
    /// </summary>
    IContextOptions Options { get; }

    /// <summary>
    ///     Creates a pair socket.
    /// </summary>
    IPairSocket CreatePairSocket();

    /// <summary>
    ///     Creates a dealer socket.
    /// </summary>
    IDealerSocket CreateDealerSocket();

    /// <summary>
    ///     Creates a router socket.
    /// </summary>
    IRouterSocket CreateRouterSocket();

    /// <summary>
    ///     Creates a pub socket.
    /// </summary>
    IPubSocket CreatePubSocket();

    /// <summary>
    ///     Creates a sub socket.
    /// </summary>
    ISubSocket CreateSubSocket();

    /// <summary>
    ///     Creates a xpub socket.
    /// </summary>
    IXPubSocket CreateXPubSocket();

    /// <summary>
    ///     Creates a xsub socket.
    /// </summary>
    IXSubSocket CreateXSubSocket();

    /// <summary>
    ///     Creates a stream socket.
    /// </summary>
    IStreamSocket CreateStreamSocket();

    /// <summary>
    ///     Terminates the context, interrupting blocking operations on its sockets
    ///     without disposing them.
    /// </summary>
    void Shutdown();

    /// <summary>
    ///     Recomputes automatic high-water marks for sockets configured with an
    ///     <see cref="AutoHwmProfile" />.
    /// </summary>
    void RecalculateAutoHwm();
}
