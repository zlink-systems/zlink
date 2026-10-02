namespace Zlink.Framework.Runtime.Backend.Contracts;

internal enum ZLinkSocketNativeEventType
{
    Connected = (int)Systems.Zlink.SocketEvent.Connected,
    ConnectDelayed = (int)Systems.Zlink.SocketEvent.ConnectDelayed,
    ConnectRetried = (int)Systems.Zlink.SocketEvent.ConnectRetried,
    Listening = (int)Systems.Zlink.SocketEvent.Listening,
    BindFailed = (int)Systems.Zlink.SocketEvent.BindFailed,
    Accepted = (int)Systems.Zlink.SocketEvent.Accepted,
    AcceptFailed = (int)Systems.Zlink.SocketEvent.AcceptFailed,
    Closed = (int)Systems.Zlink.SocketEvent.Closed,
    CloseFailed = (int)Systems.Zlink.SocketEvent.CloseFailed,
    Disconnected = (int)Systems.Zlink.SocketEvent.Disconnected,
    MonitorStopped = (int)Systems.Zlink.SocketEvent.MonitorStopped,
    HandshakeFailedNoDetail = (int)Systems.Zlink.SocketEvent.HandshakeFailedNoDetail,
    ConnectionReady = (int)Systems.Zlink.SocketEvent.ConnectionReady,
    HandshakeFailedProtocol = (int)Systems.Zlink.SocketEvent.HandshakeFailedProtocol,
    HandshakeFailedAuth = (int)Systems.Zlink.SocketEvent.HandshakeFailedAuth,
    PeerAdmissionChanged = (int)Systems.Zlink.SocketEvent.PeerWeightChanged,
}
