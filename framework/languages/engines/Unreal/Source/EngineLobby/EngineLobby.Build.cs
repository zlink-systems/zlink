using UnrealBuildTool;

public class EngineLobby : ModuleRules
{
    public EngineLobby(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Json",
            "ZLinkStreamConnector"
        });
    }
}
