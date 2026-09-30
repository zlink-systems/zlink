using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_binding_audit_b8
{
    private const int ConcurrentReceivers = 8;
    private const int PartsPerRecord = 512;
    private const int ReproRounds = 24;

    [Fact]
    public async Task nonblocking_receive_preserves_core_busy_result()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var sender = context.CreatePairSocket();
        using var receiver = context.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "binding-audit-b8-recv-busy");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);

        using (Message handshake = Message.From("ready"))
        {
            sender.Send().Message(handshake).Submit();
        }
        using (Received received = Received.Create())
            Assert.True(receiver.Recv(received, RecvFlags.DontWait));

        var sawBusyException = false;
        var sawRefusalAsNoData = false;
        for (var round = 0; round < ReproRounds && !sawBusyException
             && !sawRefusalAsNoData; round++)
        {
            for (var record = 0; record < ConcurrentReceivers; record++)
                SendMultipart(sender, PartsPerRecord, $"{round}-{record}");

            using var start = new Barrier(ConcurrentReceivers + 1);
            var workers = Enumerable.Range(0, ConcurrentReceivers)
                .Select(_ => Task.Factory.StartNew(() =>
                {
                    using var received = Received.Create();
                    start.SignalAndWait();
                    try
                    {
                        return ReceiveAttempt.FromResult(receiver.Recv(received,
                            RecvFlags.DontWait));
                    }
                    catch (ZlinkRecvException error)
                    {
                        return ReceiveAttempt.FromError(error.Result);
                    }
                }, CancellationToken.None, TaskCreationOptions.LongRunning,
                    TaskScheduler.Default))
                .ToArray();

            start.SignalAndWait();
            ReceiveAttempt[] attempts = await Task.WhenAll(workers)
                .WaitAsync(TimeSpan.FromSeconds(10));
            sawBusyException = attempts.Any(attempt =>
                attempt.Error == ZlinkRecvException.ErrorCode.Busy);
            var successfulReads = attempts.Count(attempt =>
                attempt.Received == true);
            var hiddenRefusals = attempts.Count(attempt =>
                attempt.Received == false);
            // There is one queued record for every receive attempt, so a false
            // result before all records are consumed is a Core refusal, not an
            // empty queue result.
            sawRefusalAsNoData = hiddenRefusals > 0
                && successfulReads + hiddenRefusals == ConcurrentReceivers
                && successfulReads < ConcurrentReceivers;

            using var remaining = Received.Create();
            while (receiver.Recv(remaining, RecvFlags.DontWait))
            {
            }
        }

        Assert.True(sawBusyException,
            sawRefusalAsNoData
                ? "Core returned BUSY while multipart receive staging was owned by another thread, but Recv returned false."
                : $"Core BUSY was not observed across {ReproRounds} concurrent rounds with {PartsPerRecord} parts per record.");

        using var empty = Received.Create();
        Assert.False(receiver.Recv(empty, RecvFlags.DontWait));
    }

    private static void SendMultipart(IMessageSocket sender, int partCount,
        string payload)
    {
        using Message part = Message.From(payload);
        SendSubmitOperation operation = sender.Send().Message(part);
        for (var i = 1; i < partCount; i++)
            operation = operation.Message(part);
        operation.Submit();
    }

    private readonly record struct ReceiveAttempt(
        bool? Received,
        ZlinkRecvException.ErrorCode? Error)
    {
        internal static ReceiveAttempt FromResult(bool received) =>
            new(received, null);

        internal static ReceiveAttempt FromError(
            ZlinkRecvException.ErrorCode error) => new(null, error);
    }
}
