using UnrealBuildTool;

public class CoredccSpikeEditorTarget : TargetRules
{
	public CoredccSpikeEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CoredccSpike");
	}
}
