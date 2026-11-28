using System.Text.Json;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueConfigurationTests
{
    [Theory]
    [MemberData(nameof(Scenarios))]
    public void StartupScenario(string name, string json)
    {
        using var document = JsonDocument.Parse(json);
        var scenario = document.RootElement;
        var given = scenario.GetProperty("given");
        var expected = scenario.GetProperty("expect");
        var registration = new ZLinkFrameworkRegistration();
        var builder = new ZLinkFrameworkOptionsBuilder(registration);
        builder.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
        builder.AddRelocationStore(new InMemoryRelocationStore());
        var streamName = given.TryGetProperty("streamNameUtf8Bytes", out var size)
            ? new string('가', size.GetInt32() / 3) + new string('a', size.GetInt32() % 3)
            : "login";
        var stream = builder.AddStreamNode(streamName);
        stream.Bind("tcp://127.0.0.1:0").AddSession<LoginSession>();
        stream.AuthenticationPacketName = "login";
        stream.AdmissionQueueEnabled = given.GetProperty("queueEnabled").GetBoolean();
        stream.AdmissionQueueKey = AdmissionQueueFixture.Key;
        stream.AdmissionQueueMeshName = given.GetProperty("queueMeshName").GetString();
        foreach (var mesh in given.GetProperty("meshNodes").EnumerateArray())
            builder.AddRouteMesh(mesh.GetString()!).Listen("tcp://127.0.0.1:0").Objects().Server();

        if (!expected.GetProperty("started").GetBoolean())
        {
            var error = Assert.Throws<ZLinkConfigurationException>(() =>
                ZLinkFrameworkRegistrationValidator.Validate(registration)
            );
            if (expected.GetProperty("configurationError").ValueKind == JsonValueKind.String)
                Assert.Equal(expected.GetProperty("configurationError").GetString(), error.Message);
            return;
        }

        ZLinkFrameworkRegistrationValidator.Validate(registration);
        var expectedMesh = expected.GetProperty("queueMesh").GetString();
        Assert.Equal(expectedMesh, registration.AdmissionQueue?.MeshName);
        if (expectedMesh is not null)
        {
            Assert.Equal(
                expected.GetProperty("queueSpotId").GetString(),
                AdmissionQueueMessages.SpotId(expectedMesh)
            );
            Assert.Contains(
                AdmissionQueueMessages.SpotType,
                registration.SpotNodes[expectedMesh].InstanceSpotFactories
            );
        }
    }

    public static IEnumerable<object[]> Scenarios() =>
        AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "startupScenarios");

    private sealed class LoginSession(IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }
}
