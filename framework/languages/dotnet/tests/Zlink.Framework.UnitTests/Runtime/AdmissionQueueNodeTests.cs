using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Threading.Channels;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Streams;
using static Zlink.Framework.UnitTests.AdmissionQueueMeshTests;

namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueNodeTests
{
    [Fact]
    public async Task UsedReturnsWhileNodeStateLaneIsBusy()
    {
        using var host = CreateHost(
            new ZLinkInMemoryProviderLocationStore(),
            new InMemoryRelocationStore(),
            new HandlerProbe(),
            isServer: false,
            spotClient: new ScriptedSpotClient(42, null)
        );
        await host.StartAsync();
        try
        {
            var state = await host
                .Services.GetRequiredService<ZLinkFrameworkRuntime>()
                .GetStartedStateForRoutingAsync(CancellationToken.None);
            var queue = AdmissionQueue(state.StreamNodes.Values.Single());
            var lane = StateLane(queue);
            var release = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var hold = lane.RunAsync(() => new ValueTask(release.Task));
            var submission = Task.Run(() =>
                queue.UsedAsync("login", RoutingId.From("used-session"))
            );
            try
            {
                var completion = await submission.WaitAsync(TimeSpan.FromSeconds(3));
                Assert.False(completion.IsCompleted);
            }
            finally
            {
                release.SetResult();
                await hold;
                var completion = await submission;
                await completion;
            }
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
        }
    }

    [UnsafeAccessor(UnsafeAccessorKind.Field, Name = "_admissionQueue")]
    private static extern ref ZLinkAdmissionQueueNode AdmissionQueue(ZLinkStreamNodeRuntime stream);

    [UnsafeAccessor(UnsafeAccessorKind.Field, Name = "_lane")]
    private static extern ref ZLinkStateLane StateLane(ZLinkAdmissionQueueNode queue);

    [Theory]
    [MemberData(nameof(Scenarios))]
    public async Task NodeScenario(string name, string json)
    {
        using var document = JsonDocument.Parse(json);
        var scenario = document.RootElement;
        var given = scenario.GetProperty("given");
        var steps = scenario.GetProperty("steps").EnumerateArray().ToArray();
        var expected = steps[0].GetProperty("expect");
        var epochValue = given.GetProperty("reportedEpoch");
        var epoch =
            epochValue.ValueKind == JsonValueKind.Null ? (ulong?)null : epochValue.GetUInt64();
        var globalLimit =
            given.TryGetProperty("globalLimit", out var global)
            && global.ValueKind != JsonValueKind.Null
                ? global.GetInt32()
                : (int?)null;
        var permit = given.TryGetProperty("spotPermit", out var permitValue)
            ? permitValue.GetString()
            : null;
        var probe = new HandlerProbe
        {
            FailHandler = name == "global-limit-admit-permit-before-handler",
        };
        var spot = new ScriptedSpotClient(epoch, permit);
        using var host = CreateHost(
            new ZLinkInMemoryProviderLocationStore(),
            new InMemoryRelocationStore(),
            probe,
            isServer: false,
            globalLimit,
            spot
        );
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(12));
        var token = deadline.Token;
        await host.StartAsync(token);
        try
        {
            using var peer = await Peer.ConnectAsync(host, token);
            var rid = await probe.Connected.Task.WaitAsync(token);
            var report = await spot.Reports.Reader.ReadAsync(token);
            if (epoch is not null)
                report = await spot.Reports.Reader.ReadAsync(token);
            var ticket = MakeTicket(
                given.GetProperty("ticketState").GetString()!,
                report,
                rid,
                epoch ?? 42
            );
            var metadata = ticket is null
                ? ZlinkStreamMetadata.Empty
                : ZlinkStreamMetadata.Empty.With("$zlink.admission.ticket", ticket);

            Assert.Equal(0, Volatile.Read(ref probe.Entered));
            await peer.SendRequestAsync("Login", metadata, token);
            probe.ReleaseFirst.TrySetResult();
            var (reply, payload) = await peer.ReadAsync(token);
            using (payload)
            {
                if (expected.TryGetProperty("errorCode", out var code))
                {
                    Assert.Equal(ZlinkStreamMessageKind.Error, reply.Kind);
                    Assert.Equal(
                        code.GetString(),
                        payload.RootElement.GetProperty("code").GetString()
                    );
                    if (expected.TryGetProperty("metadata", out var expectedMetadata))
                        foreach (var property in expectedMetadata.EnumerateObject())
                            Assert.Equal(
                                property.Value.GetString(),
                                reply.Metadata.Get(property.Name)
                            );
                    Assert.Equal(0, Volatile.Read(ref probe.Entered));
                }
                else
                {
                    Assert.Equal(
                        probe.FailHandler
                            ? ZlinkStreamMessageKind.Error
                            : ZlinkStreamMessageKind.Response,
                        reply.Kind
                    );
                    Assert.Equal(1, Volatile.Read(ref probe.Entered));
                }
            }

            if (expected.TryGetProperty("stageOneInFlightDelta", out var delta))
            {
                var nextReport = await spot.Reports.Reader.ReadAsync(token);
                Assert.Equal(delta.GetInt32(), nextReport.InFlight);
            }
            if (expected.TryGetProperty("admitPermitRequests", out var requests))
                Assert.Equal(requests.GetInt32(), Volatile.Read(ref spot.AdmitRequests));
            if (expected.TryGetProperty("admitPermitRequestSeq", out var requestSeq))
                Assert.Equal(requestSeq.GetUInt64(), spot.LastRequestSeq);
            if (expected.TryGetProperty("releasePermitSends", out var releases))
                Assert.Equal(releases.GetInt32(), Volatile.Read(ref spot.Releases));
            if (steps.Length > 1)
            {
                var completion = steps[1].GetProperty("expect");
                if (completion.TryGetProperty("releasePermitSends", out releases))
                {
                    await spot.Released.Task.WaitAsync(token);
                    Assert.Equal(releases.GetInt32(), Volatile.Read(ref spot.Releases));
                }
                else
                {
                    var prefix = completion.GetProperty("resolvedRequestSeq").GetUInt64();
                    do
                    {
                        report = await spot.Reports.Reader.ReadAsync(token);
                    } while (report.ResolvedRequestSeq < prefix);
                    Assert.Equal(prefix, report.ResolvedRequestSeq);
                    Assert.Empty(report.HeldPermits);
                }
            }
        }
        finally
        {
            probe.ReleaseFirst.TrySetResult();
            await host.StopAsync(CancellationToken.None);
        }
    }

    public static IEnumerable<object[]> Scenarios() =>
        AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "nodeScenarios");

    private static string? MakeTicket(
        string status,
        AdmissionReport report,
        RoutingId session,
        ulong epoch
    )
    {
        if (status == "missing")
            return null;
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var key =
            status == "bad-signature"
                ? ZLinkAdmissionTicket.Encode(new byte[32])
                : AdmissionQueueFixture.Key;
        return new ZLinkAdmissionTicket(key, null).Sign(
            new AdmissionConnection(
                report.NodeRid,
                report.NodeLifecycleGeneration,
                "login",
                ZLinkAdmissionTicket.Encode(session.ToBytes())
            ),
            status == "wrong-epoch" ? epoch + 1 : epoch,
            now + (status == "expired" ? -10000 : 30000)
        );
    }

    private sealed class ScriptedSpotClient(ulong? epoch, string? permit) : IZLinkSpotClient
    {
        internal readonly Channel<AdmissionReport> Reports =
            Channel.CreateUnbounded<AdmissionReport>();
        internal readonly TaskCompletionSource Released = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal int AdmitRequests;
        internal int Releases;
        internal ulong LastRequestSeq;

        public IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request)
        {
            Assert.Equal("$zlink.admission-queue/admission", spotId);
            return new RequestCall(this, request!);
        }

        public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message)
        {
            Assert.Equal("$zlink.admission-queue/admission", spotId);
            return new SendCall(this, Assert.IsType<AdmissionPermitRelease>(message));
        }

        private sealed class RequestCall(ScriptedSpotClient spot, object request)
            : IZLinkSpotRequestCall
        {
            public IZLinkSpotRequestCall InstanceSpot() => throw new InvalidOperationException();

            public IZLinkSpotRequestCall InstanceSpot(string type)
            {
                Assert.Equal("$zlink.AdmissionQueue", type);
                return this;
            }

            public IZLinkSpotRequestCall InMesh(string mesh)
            {
                Assert.Equal("admission", mesh);
                return this;
            }

            public IZLinkSpotRequestCall Timeout(TimeSpan timeout) =>
                throw new InvalidOperationException();

            public IZLinkSpotRequestCall Metadata(string key, string value) =>
                throw new InvalidOperationException();

            public IZLinkSpotRequestCall Metadata(ZLinkMessageMetadata metadata) =>
                throw new InvalidOperationException();

            public ZLinkRequest<TReply> Async<TReply>(
                CancellationToken cancellationToken = default
            ) => new(ValueTask.CompletedTask, ReplyAsync<TReply>());

            public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default) =>
                throw new InvalidOperationException();

            private async ValueTask<TReply> ReplyAsync<TReply>()
            {
                if (request is AdmissionReport report)
                {
                    Assert.True(spot.Reports.Writer.TryWrite(report));
                    return spot.Epoch is { } epoch
                        ? (TReply)(object)new AdmissionReported(epoch, [])
                        : throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            "Queue Spot unavailable."
                        );
                }
                var admit = Assert.IsType<AdmissionPermitRequest>(request);
                spot.LastRequestSeq = admit.RequestSeq;
                Interlocked.Increment(ref spot.AdmitRequests);
                if (spot.Permit == "noResponseWithinSpotRequestTimeout")
                {
                    await Task.Yield();
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DeadlineExceeded,
                        "Spot request timed out."
                    );
                }
                return (TReply)
                    (object)(
                        spot.Permit == "granted"
                            ? new AdmissionPermitResult(true, 31)
                            : new AdmissionPermitResult(false)
                    );
            }
        }

        private ulong? Epoch => epoch;
        private string? Permit => permit;

        private sealed class SendCall(ScriptedSpotClient spot, AdmissionPermitRelease release)
            : IZLinkSpotSendCall
        {
            public IZLinkSpotSendCall InstanceSpot() => throw new InvalidOperationException();

            public IZLinkSpotSendCall InstanceSpot(string type)
            {
                Assert.Equal("$zlink.AdmissionQueue", type);
                return this;
            }

            public IZLinkSpotSendCall InMesh(string mesh)
            {
                Assert.Equal("admission", mesh);
                return this;
            }

            public IZLinkSpotSendCall Metadata(string key, string value) =>
                throw new InvalidOperationException();

            public IZLinkSpotSendCall Metadata(ZLinkMessageMetadata metadata) =>
                throw new InvalidOperationException();

            public ValueTask Async()
            {
                Assert.Equal(31UL, release.PermitId);
                Interlocked.Increment(ref spot.Releases);
                spot.Released.TrySetResult();
                return ValueTask.CompletedTask;
            }
        }
    }
}
