using UnrealBuildTool;

public class CoredccSpikeTarget : TargetRules
{
	public CoredccSpikeTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CoredccSpike");
	}
}
