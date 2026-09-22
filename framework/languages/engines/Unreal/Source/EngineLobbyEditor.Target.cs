using UnrealBuildTool;

public class EngineLobbyEditorTarget : TargetRules
{
    public EngineLobbyEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V5;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
        ExtraModuleNames.Add("EngineLobby");
    }
}
