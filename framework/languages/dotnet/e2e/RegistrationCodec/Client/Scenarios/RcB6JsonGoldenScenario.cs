// Verifies that the default JSON serializer round-trips a typed DTO without message-specific codec registration.
using RegistrationCodec.Client.Support;
using RegistrationCodec.Shared;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

internal static class RcB6JsonGoldenScenario
{
    public static async Task RunAsync(ZLinkHttpClient requester)
    {
        var result = (await requester.Post("/codec/json-golden")
            .Async<JsonGoldenRes>()).Body;

        ZlinkStreamAssert.Ensure(result.DisplayName == "Ada Lovelace", "RC-B6 display name changed.");
        ZlinkStreamAssert.Ensure(result.Status == "ready", "RC-B6 string value changed.");
        ZlinkStreamAssert.Ensure(result.Balance == -9_223_372_036_854_775_000L, "RC-B6 int64 value changed.");
        ZlinkStreamAssert.Ensure(
            result.Payload.SequenceEqual(new byte[] { 0x00, 0x7f, 0x80, 0xff }),
            "RC-B6 bytes value changed.");
        ZlinkStreamAssert.Ensure(result.Score == 2_147_000_001, "RC-B6 int32 value changed.");
        ZlinkStreamAssert.Ensure(Math.Abs(result.Ratio - 0.125) < double.Epsilon, "RC-B6 floating value changed.");
        ZlinkStreamAssert.Ensure(result.OptionalNote is null, "RC-B6 nullable value changed.");
        ZlinkStreamAssert.Ensure(result.ContentType == "application/json", "RC-B6 did not use default JSON.");

        Console.WriteLine("scenario RC-B6 passed");
    }
}
