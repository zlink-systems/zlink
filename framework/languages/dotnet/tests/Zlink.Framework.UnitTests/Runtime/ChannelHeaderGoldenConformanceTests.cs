using System.Text.Json;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ChannelHeaderGoldenConformanceTests
{
    private static readonly string GoldenPath = Path.Combine(
        FindRepositoryRoot(),
        "framework/runtime/protocol/golden/channel-header-v1.json"
    );

    [Fact]
    public void Valid_golden_headers_decode_and_encode_exact_bytes()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(GoldenPath));
        foreach (var vector in document.RootElement.GetProperty("valid").EnumerateArray())
        {
            var name = vector.GetProperty("name").GetString();
            var expected = vector.GetProperty("header");
            var bytes = Convert.FromHexString(vector.GetProperty("hex").GetString()!);
            using var encodedInput = Message.From(bytes);

            var decoded = ZLinkEnvelopeCodec.DecodeHeader(encodedInput);
            Assert.Equal(expected.GetProperty("kind").GetInt32(), (int)decoded.Kind);
            Assert.Equal(expected.GetProperty("channel_name").GetString(), decoded.ChannelName);
            Assert.Equal(expected.GetProperty("message_name").GetString(), decoded.MessageName);
            Assert.Equal(expected.GetProperty("content_type").GetString(), decoded.ContentType);
            Assert.Equal(OptionalString(expected, "correlation_id"), decoded.CorrelationId);
            Assert.Equal(ParseDeadline(OptionalString(expected, "deadline")), decoded.Deadline);
            Assert.Equal(OptionalString(expected, "topic"), decoded.Topic);
            Assert.Equal(OptionalString(expected, "error_code"), decoded.ErrorCode);
            Assert.Equal(OptionalString(expected, "error_message"), decoded.ErrorMessage);
            Assert.Equal(OptionalString(expected, "source"), decoded.Source);
            Assert.Equal(OptionalString(expected, "flow_id"), decoded.FlowId);
            Assert.Equal(
                OptionalInteger(expected, "flow_origin"),
                decoded.FlowOrigin is { } origin ? (int)origin : null
            );
            Assert.Equal(
                ReadMetadata(expected),
                decoded.Metadata ?? new Dictionary<string, string>()
            );

            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(decoded, decoded.ContentType);
            Assert.Equal(bytes, encoded.AsReadOnlySpan().ToArray());
        }
    }

    [Fact]
    public void Invalid_golden_headers_are_protocol_errors()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(GoldenPath));
        foreach (var vector in document.RootElement.GetProperty("reject").EnumerateArray())
        {
            using var input = Message.From(
                Convert.FromHexString(vector.GetProperty("hex").GetString()!)
            );
            try
            {
                _ = ZLinkEnvelopeCodec.DecodeHeader(input);
                Assert.Fail(
                    $"Golden rejection '{vector.GetProperty("name").GetString()}' was accepted."
                );
            }
            catch (ZLinkEnvelopeProtocolException) { }
        }
    }

    private static string? OptionalString(JsonElement header, string name) =>
        header.TryGetProperty(name, out var value) ? value.GetString() : null;

    private static int? OptionalInteger(JsonElement header, string name) =>
        header.TryGetProperty(name, out var value) ? value.GetInt32() : null;

    private static DateTimeOffset? ParseDeadline(string? value) =>
        value is null
            ? null
            : DateTimeOffset.Parse(value, System.Globalization.CultureInfo.InvariantCulture);

    private static Dictionary<string, string> ReadMetadata(JsonElement header)
    {
        var result = new Dictionary<string, string>();
        if (header.TryGetProperty("metadata", out var entries))
            foreach (var entry in entries.EnumerateArray())
                result.Add(entry[0].GetString()!, entry[1].GetString()!);
        return result;
    }

    private static string FindRepositoryRoot()
    {
        for (
            var directory = new DirectoryInfo(AppContext.BaseDirectory);
            directory is not null;
            directory = directory.Parent
        )
            if (
                File.Exists(
                    Path.Combine(
                        directory.FullName,
                        "framework/runtime/protocol/golden/channel-header-v1.json"
                    )
                )
            )
                return directory.FullName;
        throw new DirectoryNotFoundException("Could not find repository root.");
    }
}
