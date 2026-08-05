using UnrealBuildTool;

public class ZLinkStreamConnectorTests : ModuleRules
{
    public ZLinkStreamConnectorTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "ZLinkStreamConnector"
        });
    }
}
