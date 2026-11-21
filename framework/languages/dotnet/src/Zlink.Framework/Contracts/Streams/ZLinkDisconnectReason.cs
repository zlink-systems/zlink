namespace Zlink.Framework.Contracts.Streams;

public enum ZLinkDisconnectReason
{
    Unknown = 0,
    HandshakeFailed = 3,
    TransportError = 4,
    ContextTerminated = 5,
}
