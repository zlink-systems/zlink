using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    private static readonly string[] ExecutionTurnScenarioIds =
    [
        "TD-A1", "TD-A2", "TD-A3", "TD-A4", "TD-A5",
        "TD-B1", "TD-B2", "TD-B3", "TD-B4",
        "TD-C1", "TD-C2", "TD-C3", "TD-C4", "TD-C5",
        "TD-D1", "TD-D2", "TD-D3",
        "TD-E1", "TD-E2", "TD-E3",
        "TD-F1", "TD-F2", "TD-F3", "TD-F4", "TD-F5", "TD-F6",
        "TD-G1", "TD-D4", "TD-D5", "TD-D6", "TD-E2A", "TD-F5A"
    ];

    [Fact]
    public void ExecutionTurn_Uses_The_Canonical_ThirtyTwo_Scenario_Inventory()
    {
        var root = Path.Combine(ResolveE2eRoot(), "AutomaticTurnDispatch");
        var featureMap = File.ReadAllText(Path.Combine(root, "feature-map.ko.md"));
        var scenarioFiles = Directory
            .EnumerateFiles(Path.Combine(root, "Client", "Scenarios"), "Td*Scenario.cs")
            .Select(Path.GetFileNameWithoutExtension)
            .ToArray();

        Assert.Contains("config-8-execution-turn.ko.md", featureMap, StringComparison.Ordinal);
        Assert.DoesNotContain("ATD-", featureMap, StringComparison.Ordinal);
        Assert.Equal(ExecutionTurnScenarioIds.Length, scenarioFiles.Length);
        foreach (var scenarioId in ExecutionTurnScenarioIds)
        {
            Assert.Contains($"| {scenarioId} |", featureMap, StringComparison.Ordinal);
            Assert.Contains(
                scenarioFiles,
                file => file!.StartsWith(
                    scenarioId.Replace("-", string.Empty),
                    StringComparison.OrdinalIgnoreCase));
        }
    }

    [Fact]
    public void ExecutionTurn_Uses_Typed_Default_Packet_Names()
    {
        var clientRoot = Path.Combine(
            ResolveE2eRoot(),
            "AutomaticTurnDispatch",
            "Client");
        var offenders = Directory.EnumerateFiles(clientRoot, "*.cs", SearchOption.AllDirectories)
            .Where(path => File.ReadAllText(path).Contains(".PacketName(", StringComparison.Ordinal))
            .Select(path => Path.GetRelativePath(clientRoot, path))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.True(
            offenders.Length == 0,
            "AutomaticTurnDispatch must infer packet names from message types: "
            + string.Join(", ", offenders));
    }

    [Fact]
    public void ExecutionTurn_Scenarios_Use_Bounded_Evidence_Waits()
    {
        var scenarioRoot = Path.Combine(
            ResolveE2eRoot(),
            "AutomaticTurnDispatch",
            "Client",
            "Scenarios");
        var offenders = Directory.EnumerateFiles(scenarioRoot, "Td*Scenario.cs")
            .Select(path => (Path: path, Source: File.ReadAllText(path)))
            .Where(item => item.Source.Contains("Task.Delay(", StringComparison.Ordinal)
                           || item.Source.Contains("AwaitEvidenceReq(", StringComparison.Ordinal))
            .Select(item => Path.GetFileName(item.Path))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.True(
            offenders.Length == 0,
            "Execution-turn scenarios must wait on bounded server evidence: "
            + string.Join(", ", offenders));
    }

    [Theory]
    [InlineData("AutomaticTurnDispatch")]
    [InlineData("PubSub")]
    [InlineData("RegistrationCodec")]
    [InlineData("SpotService")]
    public void Scenario_Files_Use_Canonical_Ids_And_Descriptive_Names(string fixture)
    {
        var root = Path.Combine(ResolveE2eRoot(), fixture);
        var scenarioIds = File.ReadLines(Path.Combine(root, "feature-map.ko.md"))
            .Where(static line => line.StartsWith("| ", StringComparison.Ordinal))
            .Select(static line => line.Split('|', StringSplitOptions.TrimEntries)[1])
            .Where(static value => value.Length >= 5 && value.Contains('-', StringComparison.Ordinal))
            .ToArray();
        var scenarioFiles = Directory
            .EnumerateFiles(Path.Combine(root, "Client", "Scenarios"), "*Scenario.cs")
            .Select(Path.GetFileNameWithoutExtension)
            .Order(StringComparer.Ordinal)
            .ToArray();

        foreach (var scenarioFile in scenarioFiles)
        {
            var matches = scenarioIds.Where(scenarioId =>
                {
                    var prefix = scenarioId.Replace("-", string.Empty, StringComparison.Ordinal);
                    return scenarioFile!.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
                           && scenarioFile.Length > prefix.Length + "Scenario".Length
                           && char.IsUpper(scenarioFile[prefix.Length]);
                })
                .OrderByDescending(static scenarioId => scenarioId.Length)
                .Take(1)
                .ToArray();
            Assert.Single(matches);
        }
    }
}
