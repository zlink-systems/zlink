using System.Text.Json;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueTicketTests
{
    [Theory]
    [MemberData(nameof(Vectors))]
    public void TicketVector(string name, string json)
    {
        using var document = JsonDocument.Parse(json);
        var vector = document.RootElement;
        var given = vector.GetProperty("given");
        string Get(string field) => given.GetProperty(field).GetString()!;
        var ticket = vector.GetProperty("ticket").GetString()!;
        if (name == "short-key")
        {
            Assert.Throws<ZLinkConfigurationException>(() =>
                new ZLinkAdmissionTicket(Get("keyBase64Url"), null)
            );
            return;
        }

        var codec = new ZLinkAdmissionTicket(Get("keyBase64Url"), Get("previousKeyBase64Url"));
        if (name == "current-key")
            Assert.Equal(
                ticket,
                codec.Sign(
                    new AdmissionConnection(
                        Get("nodeRidBase64Url"),
                        given.GetProperty("nodeLifecycleGeneration").GetUInt64(),
                        Get("streamName"),
                        Get("sessionRidBase64Url")
                    ),
                    given.GetProperty("epoch").GetUInt64(),
                    given.GetProperty("expiresAtUnixMs").GetInt64()
                )
            );
        Assert.Equal(
            vector.GetProperty("expect").GetProperty("accept").GetBoolean(),
            codec.Verify(
                ticket,
                new AdmissionConnection(
                    Get("verificationNodeRidBase64Url"),
                    given.GetProperty("verificationNodeLifecycleGeneration").GetUInt64(),
                    Get("verificationStreamName"),
                    Get("verificationSessionRidBase64Url")
                ),
                given.GetProperty("verificationEpoch").GetUInt64(),
                given.GetProperty("nowUnixMs").GetInt64()
            )
        );
    }

    public static IEnumerable<object[]> Vectors() =>
        AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "ticketVectors");
}
