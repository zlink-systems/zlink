using System.Diagnostics;
using Systems.Zlink;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Service;

VerifyCompletionFailureResult();
VerifyRawRouterLifecycleAndMultipartOwnership();

static void VerifyCompletionFailureResult()
{
    var table = new ZLinkMeshCompletionTable();
    var operation = new MeshOperationId(0, 1);
    RequestResult? terminal = null;
    Require(table.RegisterRequest(operation, (result, _) => terminal = result),
        "completion registration failed");

    table.FailAll(RequestResult.Terminated);

    Require(terminal == RequestResult.Terminated,
        $"FailAll replaced {RequestResult.Terminated} with {terminal}");
}

static void VerifyRawRouterLifecycleAndMultipartOwnership()
{
    using var context = Systems.Zlink.Zlink.CreateContext();
    var endpoint = $"inproc://m5-dotnet-raw-port-{Guid.NewGuid():N}";
    var senderRid = RoutingId.From("m5-sender");
    var receiverRid = RoutingId.From("m5-receiver");
    using var sender = new ZLinkRawRouterServicePort(context, senderRid, endpoint + "-sender");
    using var receiver = new ZLinkRawRouterServicePort(context, receiverRid, endpoint);

    RequireThrows<InvalidOperationException>(() => sender.TrySend(
        receiverRid,
        new ReadOnlyMemory<byte>[] { new byte[] { 1 } }),
        "send before Start must fail");

    receiver.Start();
    sender.Start();
    sender.Connect(endpoint, receiverRid);

    var payload = new ReadOnlyMemory<byte>[]
    {
        new byte[] { 1, 2, 3 },
        new byte[] { 4, 5 }
    };
    var stopwatch = Stopwatch.StartNew();
    var submitted = false;
    while (!submitted && stopwatch.Elapsed < TimeSpan.FromSeconds(3))
    {
        submitted = sender.TrySend(receiverRid, payload, SendFlags.DontWait);
        if (!submitted) Thread.Sleep(10);
    }
    Require(submitted, "raw ROUTER multipart send did not become ready");

    ZLinkRawRouterEnvelope? envelope = null;
    while (envelope is null && stopwatch.Elapsed < TimeSpan.FromSeconds(5))
    {
        receiver.TryReceive(out envelope);
        if (envelope is null) Thread.Sleep(10);
    }
    Require(envelope is not null, "raw ROUTER multipart receive timed out");
    var receivedEnvelope = envelope!;
    using (receivedEnvelope)
    {
        Require(receivedEnvelope.SourceRoutingId == senderRid, "source routing id was not preserved");
        Require(receivedEnvelope.Parts.Count == 2, "multipart boundary was not preserved");
        Require(receivedEnvelope.Parts[0].ToArray().SequenceEqual(new byte[] { 1, 2, 3 }),
            "first message part changed");
        Require(receivedEnvelope.Parts[1].ToArray().SequenceEqual(new byte[] { 4, 5 }),
            "second message part changed");
    }

    var requestTask = sender.RequestAsync(
        receiverRid,
        new ReadOnlyMemory<byte>[] { new byte[] { 9, 8 } },
        TimeSpan.FromSeconds(3));
    envelope = null;
    while (envelope is null && stopwatch.Elapsed < TimeSpan.FromSeconds(5))
    {
        receiver.TryReceive(out envelope);
        if (envelope is null) Thread.Sleep(10);
    }
    Require(envelope is not null, "raw ROUTER request receive timed out");
    var requestEnvelope = envelope!;
    using (requestEnvelope)
    {
        Require(requestEnvelope.CanReply, "request sequence reply context was not preserved");
        requestEnvelope.Reply(
            new ReadOnlyMemory<byte>[] { new byte[] { 7, 6, 5 } });
    }
    using var reply = requestTask.GetAwaiter().GetResult();
    Require(reply.Parts.Count == 1, "reply multipart boundary changed");
    Require(reply.Parts[0].ToArray().SequenceEqual(new byte[] { 7, 6, 5 }),
        "reply payload changed");

    sender.Dispose();
    RequireThrows<ObjectDisposedException>(() => sender.TryReceive(out _),
        "disposed port accepted receive");
}

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static void RequireThrows<TException>(Action action, string message)
    where TException : Exception
{
    try
    {
        action();
    }
    catch (TException)
    {
        return;
    }

    throw new InvalidOperationException(message);
}
