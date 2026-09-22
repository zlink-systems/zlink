using UnrealBuildTool;

public class EngineLobbyTarget : TargetRules
{
    public EngineLobbyTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V5;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
        ExtraModuleNames.Add("EngineLobby");
    }
}
