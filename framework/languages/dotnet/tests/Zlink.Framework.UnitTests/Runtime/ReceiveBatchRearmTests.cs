using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class ReceiveBatchRearmTests
{
    [Fact]
    public void ReceiveBudgetAllowsTheFirstRecordAfterItsTimeBudgetExpires()
    {
        var startedAt =
            System.Diagnostics.Stopwatch.GetTimestamp() - System.Diagnostics.Stopwatch.Frequency;

        Assert.False(ZLinkReceiveBatchBudget.IsExhausted(0, 0, startedAt));
        Assert.True(ZLinkReceiveBatchBudget.IsExhausted(1, 0, startedAt));
        Assert.False(ZLinkReceiveBatchBudget.WouldExceed(0, 0, 1, startedAt));
        Assert.True(ZLinkReceiveBatchBudget.WouldExceed(1, 0, 1, startedAt));
    }

    [Fact]
    public async Task InfiniteWaitRearmsUnreadRecordsAfterTheReceiveCountBudgetWithoutAnotherSend()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var receiver = context.CreatePairSocket();
        await using var sender = context.CreatePairSocket();
        var endpoint = $"inproc://receive-batch-rearm-{Guid.NewGuid():N}";
        receiver.Bind(endpoint);
        sender.Connect(endpoint);

        // Every record is submitted before the first wait. The second wait
        // must observe the unread level without another send creating an edge.
        const int recordCount = ZLinkReceiveBatchBudget.MaximumRecords + 1;
        for (var index = 0; index < recordCount; index++)
        {
            using var message = Message.From(index.ToString());
            sender.Send().Message(message).Submit();
        }

        using var stop = new CancellationTokenSource();
        await using var poller = ZLinkBackendSocketPoller.Create(receiver);
        var drain = DrainAsync();
        try
        {
            await drain.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            stop.Cancel();
        }

        async Task DrainAsync()
        {
            Assert.True(
                (
                    await poller.WaitAsync(Timeout.InfiniteTimeSpan, stop.Token)
                    & ZLinkBackendSocketReadiness.Readable
                ) != 0
            );
            var received = Received.Create();
            for (var index = 0; index < ZLinkReceiveBatchBudget.MaximumRecords; index++)
                Receive(index);

            Assert.True(
                ZLinkReceiveBatchBudget.IsExhausted(
                    ZLinkReceiveBatchBudget.MaximumRecords,
                    0,
                    System.Diagnostics.Stopwatch.GetTimestamp()
                )
            );
            Assert.True(
                (
                    await poller.WaitAsync(Timeout.InfiniteTimeSpan, stop.Token)
                    & ZLinkBackendSocketReadiness.Readable
                ) != 0
            );
            Receive(ZLinkReceiveBatchBudget.MaximumRecords);
            Assert.False(receiver.Recv(received, RecvFlags.DontWait));

            void Receive(int expected)
            {
                Assert.True(receiver.Recv(received, RecvFlags.DontWait));
                try
                {
                    Assert.Single(received.Parts);
                    Assert.Equal(expected.ToString(), received.Parts[0].GetString());
                }
                finally
                {
                    foreach (var part in received.Parts)
                        part.Dispose();
                }
            }
        }
    }
}
