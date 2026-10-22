using System.Buffers.Binary;
using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class StatefulWireSchemaConformanceTests
{
    [Theory]
    [InlineData(21)]
    [InlineData(22)]
    [InlineData(24)]
    [InlineData(25)]
    public void Runtime_decodes_shared_schema_headers_and_rejects_private_layouts(int commandId)
    {
        var root = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        using var fixture = JsonDocument.Parse(
            File.ReadAllText(
                Path.GetFullPath("../../runtime/protocol/golden/stateful-message-v1.json", root)
            )
        );
        foreach (var vector in fixture.RootElement.GetProperty("valid").EnumerateArray())
        {
            if (vector.GetProperty("commandId").GetInt32() != commandId)
                continue;
            var header = Convert.FromHexString(vector.GetProperty("framesHex")[0].GetString()!);
            Assert.True(
                ZLinkServiceWireCodec.TryDecodeStateful(
                    header,
                    "mesh",
                    out var record,
                    out var error
                ),
                $"{vector.GetProperty("name").GetString()}: {error}"
            );
            Assert.Equal(
                (ServiceWireConstants.Command)vector.GetProperty("commandId").GetInt32(),
                record.Command
            );
            Assert.Equal(new MeshOperationId(2, 3), record.OperationId);
            Assert.Equal(commandId is 22 or 25 ? 1UL : 0, record.Correlation);
            Assert.Equal(RoutingId.From("dst"), record.TargetNodeRid);
            Assert.Equal(0, record.MessageFollowHopCount);
            Assert.Equal(
                vector.GetProperty("name").GetString()!.Contains("metadata"),
                record.HasMetadata
            );
            if (commandId < 24)
            {
                Assert.Equal("source", record.SourceSpotId);
                Assert.Equal("target", record.TargetSpotId);
                Assert.Equal(5UL, record.TargetSpotGeneration);
            }
            else
                Assert.Equal(
                    new ActorRef("target", 5, "mesh", RoutingId.From("dst")),
                    record.TargetActor
                );

            Assert.Equal(6UL, record.TargetNodeGeneration);
            Assert.Equal(7UL, record.AuthorityOwnerGeneration);
            Assert.Equal(8UL, record.OwnerLeaseGeneration);
            var hasSourceActor =
                commandId >= 24
                && vector.GetProperty("name").GetString()!.Contains("source-metadata");
            Assert.Equal(hasSourceActor ? "source" : null, record.SourceActor?.ActorId);
            Assert.Equal(hasSourceActor ? 4UL : null, record.SourceActor?.Generation);
            {
                var encoded =
                    commandId < 24
                        ? ZLinkServiceWireCodec.EncodeSpot(
                            record.Command,
                            record.Correlation,
                            record.OperationId,
                            record.SourceSpotId,
                            record.TargetSpotId,
                            record.TargetSpotGeneration,
                            record.TargetNodeRid,
                            record.TargetNodeGeneration,
                            record.AuthorityOwnerGeneration,
                            record.OwnerLeaseGeneration,
                            record.HasMetadata,
                            record.MessageFollowHopCount
                        )
                        : ZLinkServiceWireCodec.EncodeActor(
                            record.Command,
                            record.Correlation,
                            record.OperationId,
                            record.TargetActor,
                            record.TargetNodeRid,
                            record.TargetNodeGeneration,
                            record.AuthorityOwnerGeneration,
                            record.OwnerLeaseGeneration,
                            record.HasMetadata,
                            record.MessageFollowHopCount,
                            record.SourceActor
                        );
                Assert.Equal(header, encoded);
            }
        }
        foreach (var vector in fixture.RootElement.GetProperty("invalid").EnumerateArray())
        {
            if (vector.GetProperty("commandId").GetInt32() != commandId)
                continue;
            var header = Convert.FromHexString(vector.GetProperty("framesHex")[0].GetString()!);
            Assert.False(
                ZLinkServiceWireCodec.TryDecodeStateful(header, "mesh", out _, out _),
                vector.GetProperty("name").GetString()
            );
        }
    }

    [Fact]
    public void Runtime_validates_stateful_header_boundaries()
    {
        var root = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        using var fixture = JsonDocument.Parse(
            File.ReadAllText(
                Path.GetFullPath("../../runtime/protocol/golden/stateful-message-v1.json", root)
            )
        );
        foreach (var vector in fixture.RootElement.GetProperty("valid").EnumerateArray())
        {
            var command = vector.GetProperty("commandId").GetInt32();
            var frames = vector
                .GetProperty("framesHex")
                .EnumerateArray()
                .Select(value => Convert.FromHexString(value.GetString()!))
                .ToArray();
            var header = frames[0];
            Compare(header, true);
            var request = command is 22 or 25;
            var operationOffset = request ? 13 : 5;
            var fields = new List<int>
            {
                header.Length - 36,
                header.Length - 24,
                header.Length - 16,
                header.Length - 8,
            };
            if (request)
                fields.Add(5);
            if (command >= 24 && header[operationOffset + 17] != 0)
                fields.Add(operationOffset + 24);
            foreach (var offset in fields)
            foreach (
                var value in new[]
                {
                    0UL,
                    1UL,
                    (ulong)long.MaxValue,
                    (ulong)long.MaxValue + 1,
                    ulong.MaxValue,
                }
            )
            {
                var changed = (byte[])header.Clone();
                BinaryPrimitives.WriteUInt64BigEndian(changed.AsSpan(offset), value);
                Compare(changed, value is > 0 and <= long.MaxValue);
            }
            foreach (var hop in new byte[] { 0, 8, 9, 255 })
            {
                var changed = (byte[])header.Clone();
                changed[operationOffset + 16] = hop;
                Compare(changed, hop <= 8);
            }
            foreach (
                var (high, low) in new[]
                {
                    (0UL, 0UL),
                    (0UL, ulong.MaxValue),
                    (ulong.MaxValue, 0UL),
                }
            )
            {
                var changed = (byte[])header.Clone();
                BinaryPrimitives.WriteUInt64BigEndian(changed.AsSpan(operationOffset), high);
                BinaryPrimitives.WriteUInt64BigEndian(changed.AsSpan(operationOffset + 8), low);
                Compare(changed, high != 0 || low != 0);
            }
            for (var length = 0; length < header.Length; length++)
                Compare(header[..length], false);
            Compare([.. header, 0], false);
            foreach (var invalidText in new byte[] { 0, 255 })
            {
                var changed = (byte[])header.Clone();
                changed[
                    operationOffset
                        + (
                            command < 24 ? 18
                            : header[operationOffset + 17] == 0 ? 19
                            : 18
                        )
                ] = invalidText;
                Compare(changed, false);
            }
            var forbiddenFlags = (byte[])header.Clone();
            forbiddenFlags[4] |= 2;
            Compare(forbiddenFlags, false);

            void Compare(byte[] changed, bool expected)
            {
                Assert.Equal(
                    expected,
                    ZLinkServiceWireCodec.TryDecodeStateful(changed, "mesh", out var decoded, out _)
                );
                if (expected)
                    Assert.Equal(
                        changed,
                        command < 24
                            ? ZLinkServiceWireCodec.EncodeSpot(
                                decoded.Command,
                                decoded.Correlation,
                                decoded.OperationId,
                                decoded.SourceSpotId,
                                decoded.TargetSpotId,
                                decoded.TargetSpotGeneration,
                                decoded.TargetNodeRid,
                                decoded.TargetNodeGeneration,
                                decoded.AuthorityOwnerGeneration,
                                decoded.OwnerLeaseGeneration,
                                decoded.HasMetadata,
                                decoded.MessageFollowHopCount
                            )
                            : ZLinkServiceWireCodec.EncodeActor(
                                decoded.Command,
                                decoded.Correlation,
                                decoded.OperationId,
                                decoded.TargetActor,
                                decoded.TargetNodeRid,
                                decoded.TargetNodeGeneration,
                                decoded.AuthorityOwnerGeneration,
                                decoded.OwnerLeaseGeneration,
                                decoded.HasMetadata,
                                decoded.MessageFollowHopCount,
                                decoded.SourceActor
                            )
                    );
            }
        }
    }

    [Theory]
    [InlineData(21)]
    [InlineData(22)]
    [InlineData(24)]
    [InlineData(25)]
    public void Runtime_encoder_rejects_values_outside_schema_bounds(int commandId)
    {
        var command = (ServiceWireConstants.Command)commandId;
        var rid = RoutingId.From("dst");
        var actor = new ActorRef("target", 5, "mesh", rid);
        var correlation = commandId is 22 or 25 ? 1UL : 0;
        byte[] Encode(
            ulong c,
            MeshOperationId operation,
            byte hop,
            ulong objectGeneration,
            ulong node,
            ulong authority,
            ulong lease,
            (string ActorId, ulong Generation)? source = null
        ) =>
            commandId < 24
                ? ZLinkServiceWireCodec.EncodeSpot(
                    command,
                    c,
                    operation,
                    "source",
                    "target",
                    objectGeneration,
                    rid,
                    node,
                    authority,
                    lease,
                    false,
                    hop
                )
                : ZLinkServiceWireCodec.EncodeActor(
                    command,
                    c,
                    operation,
                    actor,
                    rid,
                    node,
                    authority,
                    lease,
                    false,
                    hop,
                    source
                );
        foreach (var invalid in new[] { 0UL, (ulong)long.MaxValue + 1, ulong.MaxValue })
        {
            Assert.Throws<ArgumentOutOfRangeException>(() =>
                Encode(correlation, new(2, 3), 0, 5, invalid, 7, 8)
            );
            Assert.Throws<ArgumentOutOfRangeException>(() =>
                Encode(correlation, new(2, 3), 0, 5, 6, invalid, 8)
            );
            Assert.Throws<ArgumentOutOfRangeException>(() =>
                Encode(correlation, new(2, 3), 0, 5, 6, 7, invalid)
            );
            if (commandId < 24)
                Assert.Throws<ArgumentOutOfRangeException>(() =>
                    Encode(correlation, new(2, 3), 0, invalid, 6, 7, 8)
                );
            else
                Assert.Throws<ArgumentOutOfRangeException>(() =>
                    Encode(correlation, new(2, 3), 0, 5, 6, 7, 8, ("source", invalid))
                );
            if (correlation != 0)
                Assert.Throws<ArgumentOutOfRangeException>(() =>
                    Encode(invalid, new(2, 3), 0, 5, 6, 7, 8)
                );
        }
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            Encode(correlation, default, 0, 5, 6, 7, 8)
        );
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            Encode(correlation, new(2, 3), 9, 5, 6, 7, 8)
        );
    }

    [Fact]
    public void Runtime_text8_encoder_matches_schema_validation()
    {
        foreach (
            var text in new[]
            {
                "valid",
                "한글",
                new string('a', 255),
                "\ud800",
                "\udc00",
                "a\0b",
                "",
                new string('a', 256),
            }
        )
        {
            byte[]? expected = null;
            var generatedError = Record.Exception(() =>
                expected = ServiceWireCodec.EncodeText8(
                    new(text),
                    ServiceWireCodec.DecodeContext.Empty
                )
            );
            byte[]? actual = null;
            var runtimeError = Record.Exception(() =>
                actual = ZLinkServiceWireCodec.EncodeText(text)
            );
            Assert.Equal(generatedError is null, runtimeError is null);
            if (generatedError is null)
                Assert.Equal(expected, actual);
            var rid = RoutingId.From("dst");
            var actor = new ActorRef("target", 5, "mesh", rid);
            foreach (var commandId in new[] { 21, 22, 24, 25 })
            {
                var command = (ServiceWireConstants.Command)commandId;
                var correlation = commandId is 22 or 25 ? 1UL : 0;
                var statefulError = Record.Exception(() =>
                {
                    if (commandId < 24)
                        _ = ZLinkServiceWireCodec.EncodeSpot(
                            command,
                            correlation,
                            new(2, 3),
                            text,
                            "target",
                            5,
                            rid,
                            6,
                            7,
                            8,
                            false
                        );
                    else
                        _ = ZLinkServiceWireCodec.EncodeActor(
                            command,
                            correlation,
                            new(2, 3),
                            actor,
                            rid,
                            6,
                            7,
                            8,
                            false,
                            0,
                            (text, 4)
                        );
                });
                Assert.Equal(generatedError is null, statefulError is null);
            }
        }
    }
}
