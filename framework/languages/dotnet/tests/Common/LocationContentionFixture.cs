using System.Text.Json;
using Zlink.Framework.Tests.Common;

namespace Zlink.Framework.Tests;

internal static class LocationContentionFixture
{
    public static JsonElement ReadScenario(string name)
    {
        var path = Path.Combine(
            FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/conformance/location-contention-v1.json"
        );
        using var fixture = JsonDocument.Parse(File.ReadAllBytes(path));
        return fixture.RootElement.GetProperty(name).Clone();
    }
}
