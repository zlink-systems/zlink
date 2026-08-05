using Systems.Zlink;
using System.Text.Json;

var version = Zlink.Version();
if (version != (@CORE_VERSION_TUPLE@))
    throw new InvalidOperationException($"Expected Core @CORE_VERSION@, loaded {version}.");

var loadedRuntime = File.ReadLines("/proc/self/maps")
    .Select(line => line.Split(' ', StringSplitOptions.RemoveEmptyEntries).LastOrDefault())
    .FirstOrDefault(path => path?.Contains("libzlink.so", StringComparison.Ordinal) == true);
if (loadedRuntime is null)
    throw new InvalidOperationException("The loaded Core runtime was not visible in /proc/self/maps.");

Func<IPairSocket, SendOperation> multipartSend = static socket => socket.Send();
Func<SendOperation, Message, SendSubmitOperation> appendPart =
    static (operation, part) => operation.Message(part);
Func<Received, IReadOnlyList<Message>> multipartReceive =
    static received => received.Parts;

Func<ISocket, ISocketMonitor> monitorOpen = static socket => socket.MonitorOpen();
Func<ISocketMonitor, MonitorStatus> monitorStatus = static monitor => monitor.Status();
Func<MonitorStatus, bool> monitorReady = static status => status.IsReady;

Func<IStreamSocket, RoutingId, SendOperation> streamSend =
    static (socket, routingId) => socket.Send(routingId);
Func<IStreamSocket, Received, bool> streamReceive =
    static (socket, received) => socket.Recv(received);
Action<IStreamSocket, StreamPacketHandler> streamDispatch =
    static (socket, handler) => socket.OnPacket(handler);

Action<IContext> shutdown = static context => context.Shutdown();
Action<ISocket> socketClose = static socket => socket.Close();
Action<ISocketMonitor> monitorClose = static monitor => monitor.Close();
Func<SubmitResult, bool> submitSucceeded = static result => result == SubmitResult.Ok;

_ = new Delegate[]
{
    multipartSend, appendPart, multipartReceive,
    monitorOpen, monitorStatus, monitorReady,
    streamSend, streamReceive, streamDispatch,
    shutdown, socketClose, monitorClose, submitSucceeded
};

Console.WriteLine(JsonSerializer.Serialize(new
{
    version = $"{version.Major}.{version.Minor}.{version.Patch}",
    loadedRuntime
}));
