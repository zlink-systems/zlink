using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_raw_capability
{
    private delegate bool StreamPartReceiver(
        IStreamSocket socket,
        out RoutingId? sourceRoutingId,
        out Message? part,
        out bool hasMore,
        RecvFlags flags);

    [Fact]
    public void public_assembly_exposes_required_raw_capabilities()
    {
        Func<IPairSocket, SendOperation> multipartSend =
            static socket => socket.Send();
        Func<SendOperation, Message, SendSubmitOperation> firstPart =
            static (operation, part) => operation.Message(part);
        Func<SendSubmitOperation, Message, SendSubmitOperation> nextPart =
            static (operation, part) => operation.Message(part);
        Func<Received, IReadOnlyList<Message>> multipartReceive =
            static received => received.Parts;

        Func<ISocket, ISocketMonitor> monitorOpen =
            static socket => socket.MonitorOpen();
        Func<ISocketMonitor, MonitorStatus> monitorStatus =
            static monitor => monitor.Status();
        Func<MonitorStatus, bool> ready = static status => status.IsReady;

        Func<IStreamSocket, RoutingId, SendOperation> streamSend =
            static (socket, routingId) => socket.Send(routingId);
        Func<IStreamSocket, Received, bool> streamReceive =
            static (socket, received) => socket.Recv(received);
        StreamPartReceiver streamPartReceive =
            static (IStreamSocket socket, out RoutingId? routingId, out Message? part,
                out bool hasMore, RecvFlags flags) => socket.RecvPart(
                    out routingId, out part, out hasMore, flags);
        Action<IStreamSocket, StreamPacketHandler> streamDispatch =
            static (socket, handler) => socket.OnPacket(handler);

        Action<IContext> shutdown = static context => context.Shutdown();
        Action<ISocket> socketClose = static socket => socket.Close();
        Action<ISocketMonitor> monitorClose = static monitor => monitor.Close();

        Assert.All(new Delegate[]
        {
            multipartSend, firstPart, nextPart, multipartReceive,
            monitorOpen, monitorStatus, ready,
            streamSend, streamReceive, streamPartReceive, streamDispatch,
            shutdown, socketClose, monitorClose
        }, capability => Assert.NotNull(capability));
        Assert.Equal(SocketEvent.ConnectionReady, SocketEvent.ConnectionReady);
    }
}
