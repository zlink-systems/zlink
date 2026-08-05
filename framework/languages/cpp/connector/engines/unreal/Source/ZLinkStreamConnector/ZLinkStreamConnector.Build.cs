using UnrealBuildTool;

public class ZLinkStreamConnector : ModuleRules
{
    public ZLinkStreamConnector(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });
        PrivateDependencyModuleNames.AddRange(new string[] {});
    }
}
