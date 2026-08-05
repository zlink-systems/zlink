// Verifies SM-A13 SpotId UTF-8 boundaries and exact equality.
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA13SpotIdBoundaryScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var result = (await playA.Post("/spot/id-boundary")
                .Async<SpotIdBoundaryRes>()).Body;

        ZlinkStreamAssert.Ensure(
            result.ValidIds.Length == 6
                && result.ValidIds.SequenceEqual(result.FoundIds, StringComparer.Ordinal)
                && result.StateValues.All(value => value == 1)
                && result.ExactEquality,
            "SM-A13 valid Spot IDs did not preserve exact identity and state.");
        ZlinkStreamAssert.Ensure(
            string.Equals(result.InvalidErrorKind, "InvalidOperation", StringComparison.Ordinal)
                || string.Equals(result.InvalidErrorKind, "ArgumentException", StringComparison.Ordinal)
                || string.Equals(result.InvalidErrorKind, "InvalidDataException", StringComparison.Ordinal),
            $"SM-A13 256-byte Spot ID returned unexpected error '{result.InvalidErrorKind}'.");
        ZlinkStreamAssert.Ensure(
            result.InvalidFactoryCalls == 0,
            "SM-A13 invalid Spot ID invoked the factory.");
        Console.WriteLine("operation SpotService.sm-a13 passed");
    }
}
