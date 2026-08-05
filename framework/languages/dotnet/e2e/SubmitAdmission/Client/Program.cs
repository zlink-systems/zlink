using SubmitAdmission.Client;
using SubmitAdmission.Shared;
using System.Text.Json;
using Zlink.HttpClient;

var options = ClientOptions.Parse(args);
var runner = new ScenarioRunner(options);
await runner.RunAsync();

internal sealed class ScenarioRunner(ClientOptions options)
{
    private static readonly string[] Implemented =
    [
        "SA-E2E-01", "SA-E2E-05", "SA-E2E-07", "SA-E2E-08",
        "SA-E2E-09", "SA-E2E-14", "SA-E2E-20"
    ];

    public async Task RunAsync()
    {
        var selected = options.Scenario == "all"
            ? Implemented
            : options.Scenario.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        foreach (var scenario in selected)
        {
            switch (scenario)
            {
                case "SA-E2E-01": await FastPathAsync(); break;
                case "SA-E2E-05": await MissingAndDisconnectedAsync(); break;
                case "SA-E2E-07": await CancellationAsync(); break;
                case "SA-E2E-08": await NodeParityAsync(); break;
                case "SA-E2E-09": await ChannelAsync(); break;
                case "SA-E2E-14": await FanoutWithoutSubscriberAsync(); break;
                case "SA-E2E-20": await HandlerCompletionSeparationAsync(); break;
                default:
                    throw new InvalidOperationException(
                        $"{scenario} is not implemented by the .NET Config 13 runner; see feature-map.ko.md.");
            }

            Console.WriteLine($"{scenario} PASS");
        }

        Console.WriteLine($"submit-admission process scenarios passed={selected.Length}");
    }

    private async Task FastPathAsync()
    {
        var remote = await SubmitNodeAsync(options.TargetRid, Next("fast-remote"));
        AssertStatus(remote, "Submitted");
        var channel = await SubmitChannelAsync(Next("fast-channel"));
        AssertStatus(channel, "Submitted");
    }

    private async Task MissingAndDisconnectedAsync()
    {
        for (var attempt = 0; attempt < 100; attempt++)
        {
            var missing = await SubmitNodeAsync("submit-missing", Next($"missing-{attempt}"));
            AssertStatus(missing, "TargetNotFound");
        }

        await PostAsync($"{options.CallerUrl}/admin/disconnect");
        for (var attempt = 0; attempt < 100; attempt++)
        {
            var disconnected = await SubmitNodeAsync(options.TargetRid, Next($"disconnected-{attempt}"));
            AssertStatus(disconnected, "RouteNotConnected");
        }
        await PostAsync($"{options.CallerUrl}/admin/connect");
        await WaitRouteReadyAsync();
    }

    private async Task CancellationAsync()
    {
        var valid = Next("pre-cancelled");
        var cancelled = await PostJsonAsync<CancellationResponse>(
            $"{options.CallerUrl}/submit/pre-cancelled/{options.TargetRid}", valid);
        Require(cancelled.Outcome == "Cancelled" && cancelled.TerminalCount == 1,
            $"pre-cancelled terminal mismatch: {cancelled}");

        var invalidId = $"invalid-{Guid.NewGuid():N}";
        var invalid = await PostJsonAsync<CancellationResponse>(
            $"{options.CallerUrl}/submit/invalid-pre-cancelled/{options.TargetRid}?operationId={invalidId}",
            new { });
        Require(invalid.Outcome == "Invalid" && invalid.InvalidInvocationCount == 1,
            $"invalid/cancel precedence mismatch: {invalid}");
        Require(invalid.ExceptionType != nameof(OperationCanceledException),
            "invalid input was hidden by pre-cancellation");
    }

    private async Task NodeParityAsync()
    {
        var localMessage = Next("local-node");
        var remoteMessage = Next("remote-node");
        var local = await SubmitNodeAsync(options.CallerRid, localMessage);
        var remote = await SubmitNodeAsync(options.TargetRid, remoteMessage);
        AssertStatus(local, "Submitted");
        AssertStatus(remote, "Submitted");
        var localEvidence = await WaitEvidenceAsync(
            options.CallerUrl, localMessage.OperationId, value => value.HandlerCompletedCount == 1);
        var remoteEvidence = await WaitEvidenceAsync(
            options.TargetUrl, remoteMessage.OperationId, value => value.HandlerCompletedCount == 1);
        Require(localEvidence.HandlerEnteredCount == 1 && remoteEvidence.HandlerEnteredCount == 1,
            "local and remote direct dispatch did not each enter one handler");

        var objectClient = await PostJsonAsync<NodeTargetOutcome>(
            $"{options.CallerUrl}/submit/node-target-outcome/{options.ObjectClientRid}",
            new { });
        Require(
            objectClient.Send == "NotFound"
            && objectClient.Request == "NotFound",
            $"Object Client Node direct outcome mismatch: {objectClient}");
        Require(
            objectClient.PeerCountBefore == objectClient.PeerCountAfter
            && objectClient.ReadyPeerCountBefore
                == objectClient.ReadyPeerCountAfter,
            $"Object Client Node direct changed the route set: {objectClient}");
    }

    private async Task ChannelAsync()
    {
        var result = await SubmitChannelAsync(Next("channel"));
        AssertStatus(result, "Submitted");
    }

    private async Task FanoutWithoutSubscriberAsync()
    {
        var result = await PostJsonAsync<SubmitResponse>(
            $"{options.PublisherUrl}/submit/fanout", Next("fanout-zero"));
        AssertStatus(result, "Submitted");
    }

    private async Task HandlerCompletionSeparationAsync()
    {
        await PostAsync($"{options.TargetUrl}/gate/close");
        var message = Next("handler-gate");
        var result = await SubmitNodeAsync(options.TargetRid, message);
        AssertStatus(result, "Submitted");

        var before = await WaitEvidenceAsync(
            options.TargetUrl,
            message.OperationId,
            value => value.HandlerEnteredCount == 1);
        Require(before.HandlerCompletedCount == 0,
            "handler completed before its application gate was released");
        await PostAsync($"{options.TargetUrl}/gate/open");
        var after = await WaitEvidenceAsync(
            options.TargetUrl,
            message.OperationId,
            value => value.HandlerCompletedCount == 1);
        Require(result.TerminalCount == 1 && after.HandlerEnteredCount == 1,
            "submit or handler terminal count was not exactly one");
    }

    private async Task<SubmitResponse> SubmitNodeAsync(string targetRid, AdmissionMessage message) =>
        await PostJsonAsync<SubmitResponse>(
            $"{options.CallerUrl}/submit/node/{targetRid}", message);

    private async Task<SubmitResponse> SubmitChannelAsync(AdmissionMessage message) =>
        await PostJsonAsync<SubmitResponse>(
            $"{options.CallerUrl}/submit/channel", message);

    private async Task WaitRouteReadyAsync()
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        Exception? last = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                using var http = CreateClient(options.CallerUrl);
                if ((await http.Get($"/ready/{options.TargetRid}").AsyncRaw()).Status is >= 200 and < 300)
                    return;
            }
            catch (Exception exception)
            {
                last = exception;
            }
            await Task.Delay(50);
        }
        throw new TimeoutException("Route did not become ready within 3 seconds.", last);
    }

    private async Task<OperationEvidence> WaitEvidenceAsync(
        string baseUrl,
        string operationId,
        Func<OperationEvidence, bool> condition)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTimeOffset.UtcNow < deadline)
        {
            using var http = CreateClient(baseUrl);
            var response = await http.Get($"/evidence/{operationId}").AsyncRaw();
            if (response.Status is >= 200 and < 300)
            {
                var evidence = JsonSerializer.Deserialize<OperationEvidence>(
                    response.Body,
                    new JsonSerializerOptions(JsonSerializerDefaults.Web));
                if (evidence is not null && condition(evidence)) return evidence;
            }
            await Task.Delay(25);
        }
        throw new TimeoutException($"Evidence was not observed for {operationId} within 3 seconds.");
    }

    private async Task PostAsync(string url)
    {
        var (baseUrl, path) = SplitUrl(url);
        using var http = CreateClient(baseUrl);
        var response = await http.Post(path).AsyncRaw();
        Require(response.Status is >= 200 and < 300,
            $"HTTP POST failed. url={url}, status={response.Status}");
    }

    private async Task<T> PostJsonAsync<T>(string url, object body)
    {
        var (baseUrl, path) = SplitUrl(url);
        using var http = CreateClient(baseUrl);
        return (await http.Post(path).Body(body).Async<T>()).Body;
    }

    private static ZLinkHttpClient CreateClient(string baseUrl) =>
        ZLinkHttpClient.Create(baseUrl)
            .Timeout(TimeSpan.FromSeconds(3))
            .Build();

    private static (string BaseUrl, string Path) SplitUrl(string url)
    {
        var uri = new Uri(url, UriKind.Absolute);
        return (uri.GetLeftPart(UriPartial.Authority), uri.PathAndQuery);
    }

    private static AdmissionMessage Next(string marker) =>
        new($"{marker}-{Guid.NewGuid():N}", 1, new string('x', 128));

    private static void AssertStatus(SubmitResponse response, string expected) =>
        Require(response.Status == expected
                && response.PublicInvocationCount == 1
                && response.TerminalCount == 1,
            $"submit result mismatch: expected={expected}, actual={response}");

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
