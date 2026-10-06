using Systems.Zlink.Stream.Connector.Contracts;

internal static class Receiving
{
    private sealed record LeaderboardUpdate(int Rank);

    private sealed record Ready(string Stage);

    private sealed record MatchFound(string MatchId);

    private sealed record OrderChanged(string Status);

    private sealed record ReceivingStage(string Stage);

    public static async Task RunAsync(Uri endpoint)
    {
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions { Endpoint = endpoint }
        );
        var handled = 0;
        var frames = 0;
        var running = true;
        void RenderFrame()
        {
            frames++;
            running = false;
        }
        var subscription = connector.On<LeaderboardUpdate>(
            (_, _) =>
            {
                handled++;
                return ValueTask.CompletedTask;
            }
        );
        await connector.Connect.Async();
        await connector.Send(new ReceivingStage("pump")).Async();
        await connector.WaitFor<Ready>().Async();
        // --8<-- [start:receiving-pump]
        while (running)
        {
            await connector.Dispatch.Async();
            RenderFrame();
        }
        // --8<-- [end:receiving-pump]
        // --8<-- [start:receiving-unsubscribe]
        subscription.Dispose();
        // --8<-- [end:receiving-unsubscribe]
        await connector.Send(new ReceivingStage("unsubscribed")).Async();
        await connector.WaitFor<Ready>().Async();
        await connector.Dispatch.Async();
        await connector.Send(new ReceivingStage("match")).Async();
        // --8<-- [start:receiving-wait]
        var found = await connector
            .WaitFor<MatchFound>()
            .Where(message => message.Payload!.MatchId == "match-7f3a")
            .Timeout(TimeSpan.FromSeconds(30))
            .Async();
        // --8<-- [end:receiving-wait]
        // --8<-- [start:receiving-sequence]
        await connector.ExpectNone<OrderChanged>().Within(TimeSpan.FromMilliseconds(100)).Async();
        await connector.Send(new ReceivingStage("orders")).Async();
        var steps = await connector
            .WaitForSequence<OrderChanged>()
            .Expect(message => message.Payload!.Status == "paid")
            .Expect(message => message.Payload!.Status == "shipped")
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        // --8<-- [end:receiving-sequence]
        // --8<-- [start:receiving-count]
        var count = connector.ReceivedCount("LeaderboardUpdate");
        // --8<-- [end:receiving-count]
        if (
            handled != 1
            || frames != 1
            || count != 2
            || found.Payload.MatchId != "match-7f3a"
            || steps.Count != 2
        )
            throw new InvalidOperationException(
                "Receiving tutorial result did not match expected messages."
            );
        Console.WriteLine(
            $"receiving: handler={handled}, frames={frames}, match={found.Payload.MatchId}, sequence={string.Join(",", steps.Select(m => m.Payload.Status))}, count={count}"
        );
    }
}
