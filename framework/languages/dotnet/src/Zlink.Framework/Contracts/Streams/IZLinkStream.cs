namespace Zlink.Framework.Contracts.Streams;

public interface IZLinkStream
{
    string SessionId { get; }

    RoutingId? RoutingId { get; }

    string? LocalAddr { get; }

    string? RemoteAddr { get; }

    /// <summary>
    ///     Writes a stream payload through the framework message boundary.
    /// </summary>
    bool Write(
        ZLinkMessage payload,
        SendFlags flags = SendFlags.None);

    ValueTask CloseAsync();
}
