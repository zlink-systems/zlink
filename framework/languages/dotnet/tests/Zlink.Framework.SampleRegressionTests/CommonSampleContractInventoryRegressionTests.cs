using System.Text.RegularExpressions;
using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void Common_Sample_Message_Inventory_Is_Present_In_DotNet_Source()
    {
        var repositoryRoot = Path.GetFullPath(Path.Combine(
            ResolveDotnetRoot(), "..", "..", ".."));
        var commonSampleRoot = Path.Combine(
            repositoryRoot, "framework", "doc", "framework", "common", "sample");
        var contracts = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["TicTacToe"] = Path.Combine("tictactoe", "README.ko.md"),
            ["Bingo"] = Path.Combine("bingo", "README.ko.md"),
            ["SupportChat"] = Path.Combine("supportchat", "README.ko.md"),
            ["ShoppingMall"] = Path.Combine("event", "shoppingmall.ko.md"),
            ["DeliveryDispatch"] = Path.Combine("deliverydispatch", "README.ko.md"),
            ["GameQuest"] = Path.Combine("event", "gamequest.ko.md"),
            ["ZoneWorld"] = Path.Combine("zoneworld", "README.ko.md")
        };

        var declaration = new Regex(
            @"(?m)^message\s+(?<name>[A-Za-z0-9_]+)\s*\{",
            RegexOptions.CultureInvariant);
        var missing = new List<string>();
        foreach (var (sampleName, relativeContract) in contracts)
        {
            var document = File.ReadAllText(Path.Combine(commonSampleRoot, relativeContract));
            var source = string.Join(
                Environment.NewLine,
                EnumerateSourceFiles(ResolveSampleRoot(sampleName)).Select(File.ReadAllText));
            foreach (Match match in declaration.Matches(document))
            {
                var messageName = match.Groups["name"].Value;
                if (!source.Contains(messageName, StringComparison.Ordinal)
                    && !HasShoppingMallPortAlias(sampleName, messageName, source))
                    missing.Add($"{sampleName}:{messageName}");
            }
        }

        Assert.True(
            missing.Count == 0,
            "Common sample message declarations missing from .NET source: "
            + string.Join(", ", missing));
    }

    private static bool HasShoppingMallPortAlias(
        string sampleName,
        string messageName,
        string source)
    {
        if (!string.Equals(sampleName, "ShoppingMall", StringComparison.Ordinal)) return false;

        var alias = messageName switch
        {
            "ReserveInventoryReq" => "ReserveInventoryCommand",
            "ReserveInventoryRes" => "ReserveInventoryResult",
            "ReleaseInventoryReq" => "ReleaseInventoryCommand",
            "ReleaseInventoryRes" => "ReleaseInventoryResult",
            "AuthorizePaymentReq" => "AuthorizePaymentCommand",
            "AuthorizePaymentRes" => "AuthorizePaymentResult",
            _ => null
        };
        return alias is not null && source.Contains(alias, StringComparison.Ordinal);
    }
}
