using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_raw_capability
{
    [Fact]
    public void public_assembly_exposes_required_pull_capabilities()
    {
        Func<IPairSocket, SendOperation> send = static socket => socket.Send();
        Func<SendOperation, Message, SendSubmitOperation> firstPart =
            static (operation, part) => operation.Message(part);
        Func<Received, IReadOnlyList<Message>> receive =
            static received => received.Parts;
        Func<IStreamSocket, RoutingId, SendOperation> streamSend =
            static (socket, routingId) => socket.Send(routingId);
        Func<IStreamSocket, StreamPacket, bool> streamReceive =
            static (socket, packet) => socket.RecvPacket(packet);
        Func<ISocketMonitor, MonitorEvent?> monitorReceive =
            static monitor => monitor.Recv(RecvFlags.DontWait);
        Func<IZlinkTimer, ulong?> timerReceive =
            static timer => timer.Recv(RecvFlags.DontWait);

        Assert.All(new Delegate[]
        {
            send, firstPart, receive, streamSend, streamReceive,
            monitorReceive, timerReceive
        }, capability => Assert.NotNull(capability));
    }
}
