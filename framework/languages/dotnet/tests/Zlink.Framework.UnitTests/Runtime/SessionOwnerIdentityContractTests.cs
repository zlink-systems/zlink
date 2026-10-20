using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Session-Actor binding §4: every runtime encodes the stored Session owner identity
/// unchanged in commands 51 and 54, and the receiving Session owner accepts only an
/// exact match with its current values. The same bytes are checked by the Java,
/// Node.js and C++ suites.
/// </summary>
public sealed class SessionOwnerIdentityContractTests
{
    private static readonly ServiceWireCodec.DecodeContext Context = new(
        null,
        null,
        null,
        4294966774L,
        uint.MaxValue
    );

    [Fact]
    public void Commands_carry_the_descriptor_identity_and_the_Session_owner_compares_it_exactly()
    {
        using var fixture = JsonDocument.Parse(
            File.ReadAllText(
                Path.Combine(
                    Zlink.Framework.Tests.Common.FrameworkTestEnvironment.GetRepoRoot(),
                    "framework/runtime/protocol/fixtures/session-owner-identity-v1.json"
                )
            )
        );
        var root = fixture.RootElement;
        var owner = root.GetProperty("sessionOwner");
        var session = root.GetProperty("session");
        var actor = root.GetProperty("actor");
        var local = new ZLinkServiceWireCodec.RequestSourceFence(
            owner.GetProperty("ownerId").GetString()!,
            ulong.Parse(owner.GetProperty("leaseGeneration").GetString()!),
            RoutingId.From(Convert.FromHexString(owner.GetProperty("nodeRidHex").GetString()!)),
            ulong.Parse(owner.GetProperty("nodeGeneration").GetString()!)
        );
        var sessionRid = Convert.FromHexString(session.GetProperty("sessionRidHex").GetString()!);
        var bindingGeneration = ulong.Parse(session.GetProperty("bindingGeneration").GetString()!);
        var actorId = actor.GetProperty("actorId").GetString()!;
        var objectGeneration = ulong.Parse(actor.GetProperty("objectGeneration").GetString()!);

        // The Actor owner encodes the stored identity unchanged.
        var replaced = ZLinkServiceWireCodec.EncodeBoundSessionReplaced(
            new ZLinkServiceWireCodec.BoundSessionReplacedRecord(
                new ZLinkServiceWireCodec.BoundSessionReplacedActorAuthority(
                    actorId,
                    objectGeneration,
                    RoutingId.From(
                        Convert.FromHexString(actor.GetProperty("ownerNodeRidHex").GetString()!)
                    ),
                    ulong.Parse(actor.GetProperty("ownerNodeGeneration").GetString()!),
                    ulong.Parse(actor.GetProperty("authorityOwnerGeneration").GetString()!),
                    ulong.Parse(actor.GetProperty("ownerLeaseGeneration").GetString()!)
                ),
                new ZLinkServiceWireCodec.BoundSessionReplacedRetiredSession(
                    local.NodeRid,
                    local.NodeGeneration,
                    local.OwnerId,
                    local.LeaseGeneration,
                    RoutingId.From(sessionRid),
                    bindingGeneration
                )
            )
        );
        Assert.Equal(Hex(root.GetProperty("accepted"), "command51Hex"), replaced);
        var close = ServiceWireCodec.EncodeBoundSessionClose54(
            new ServiceWireCodec.BoundSessionClose54(
                0,
                new(ulong.Parse(root.GetProperty("correlation").GetString()!)),
                new(new(actorId), new(objectGeneration)),
                new(
                    new(local.NodeRid.ToBytes().ToArray()),
                    new(local.NodeGeneration),
                    new(local.OwnerId),
                    new(local.LeaseGeneration),
                    new(sessionRid),
                    new(bindingGeneration)
                )
            ),
            Context
        )[0];
        Assert.Equal(Hex(root.GetProperty("accepted"), "command54Hex"), close);

        // The Session owner accepts only an exact match with its current values.
        Assert.True(Identifies(local, root.GetProperty("accepted"), "command51Hex"));
        Assert.True(Identifies(local, root.GetProperty("accepted"), "command54Hex"));
        foreach (var rejected in root.GetProperty("rejected").EnumerateArray())
        {
            Assert.False(Identifies(local, rejected, "command51Hex"));
            Assert.False(Identifies(local, rejected, "command54Hex"));
        }
    }

    private static byte[] Hex(JsonElement commands, string field) =>
        Convert.FromHexString(commands.GetProperty(field).GetString()!);

    [Fact]
    public void A_missing_Session_owner_descriptor_travels_as_Unavailable()
    {
        using var fixture = JsonDocument.Parse(
            File.ReadAllText(
                Path.Combine(
                    Zlink.Framework.Tests.Common.FrameworkTestEnvironment.GetRepoRoot(),
                    "framework/runtime/protocol/fixtures/session-owner-identity-v1.json"
                )
            )
        );
        var unavailable = fixture.RootElement.GetProperty("descriptorUnavailable");
        var terminal = int.Parse(unavailable.GetProperty("terminalResult").GetString()!);
        var code = int.Parse(unavailable.GetProperty("failureCode").GetString()!);
        Assert.Equal("Unavailable", unavailable.GetProperty("errorKind").GetString());

        var (result, failureCode) = ZLinkRequestFailureMapper.TargetFailureReply(
            new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, "no descriptor")
        );
        Assert.Equal(terminal, (int)result);
        Assert.Equal(code, (int)failureCode);
        var received = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateCompletionException(result, code, "bind")
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, received.Kind);
    }

    private static bool Identifies(
        ZLinkServiceWireCodec.RequestSourceFence local,
        JsonElement commands,
        string field
    )
    {
        var bytes = Hex(commands, field);
        if (field == "command51Hex")
        {
            Assert.True(
                ZLinkServiceWireCodec.TryDecodeBoundSessionReplaced(bytes, out var record, out _)
            );
            var retired = record.RetiredSession;
            return local.IdentifiesSessionOwner(
                retired.SessionOwnerNodeRid,
                retired.SessionOwnerNodeGeneration,
                retired.SessionOwnerId,
                retired.SessionOwnerLeaseGeneration
            );
        }
        var session = ServiceWireCodec.DecodeBoundSessionClose54([bytes], Context).Session;
        return local.IdentifiesSessionOwner(
            RoutingId.From(session.SessionOwnerNodeRid.Value),
            session.SessionOwnerNodeGeneration.Value,
            session.SessionOwnerId.Value,
            session.SessionOwnerLeaseGeneration.Value
        );
    }
}
