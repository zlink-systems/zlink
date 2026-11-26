using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RelocationCompletionRecordTests
{
    [Fact]
    public async Task Shared_golden_round_trips_and_rejects_invalid_completion_records()
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/protocol/golden/relocation-completion-record-v1.json"
        );
        using var fixture = JsonDocument.Parse(File.ReadAllBytes(path));
        foreach (var item in fixture.RootElement.GetProperty("cases").EnumerateArray())
        {
            var bytes = item.GetProperty("bytesBase64").GetBytesFromBase64();
            using var input = new MemoryStream(bytes);
            var hash = SHA256.HashData(bytes);
            if (item.GetProperty("expect").GetString() == "reject")
            {
                await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
                    ZLinkRelocationCompletionRecord.DecodeAsync(input, hash, default).AsTask()
                );
                continue;
            }
            var record = await ZLinkRelocationCompletionRecord.DecodeAsync(input, hash, default);
            Assert.Equal(Guid.Parse("0f1e2d3c-4b5a-4968-8778-a1b2c3d4e5f6"), record.RelocationId);
            using var output = new MemoryStream();
            await record.EncodeAsync(output, default);
            Assert.Equal(bytes, output.ToArray());
            Assert.Equal(
                item.GetProperty("sha256").GetString(),
                Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant()
            );
            Assert.Equal(
                record.Entries.Count.ToString(CultureInfo.InvariantCulture),
                item.GetProperty("completedResults").ToString()
            );
            Assert.Equal(
                record.PendingDeliveries.ToString(CultureInfo.InvariantCulture),
                item.GetProperty("pendingDeliveries").ToString()
            );
        }
    }
}
