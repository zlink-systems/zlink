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
            Assert.Equal(6UL, record.TargetNodeGeneration);
            Assert.Equal(7UL, record.AuthorityOwnerGeneration);
            Assert.Equal(8UL, record.OwnerLeaseGeneration);
            var hasSourceActor =
                commandId >= 24
                && vector.GetProperty("name").GetString()!.Contains("source-metadata");
            Assert.Equal(hasSourceActor ? "source" : null, record.SourceActor?.ActorId.Value);
            Assert.Equal(hasSourceActor ? 4UL : null, record.SourceActor?.Generation?.Value);
            if (commandId < 24 || !hasSourceActor)
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
                            record.MessageFollowHopCount
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
}
