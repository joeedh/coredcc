using System.IO;
using UnrealBuildTool;

// Links the coredcc libraries that tools/unreal-spike.mjs built with Unreal's flags, as listed
// in build/unreal-spike/libs.txt.
public class CoredccSpike : ModuleRules
{
	public CoredccSpike(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		bUseRTTI = false;
		bEnableExceptions = true;
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "UnrealEd", "InteractiveToolsFramework", "EditorInteractiveToolsFramework", "RHI", "RenderCore",
		});

		string Repo = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", ".."));
		PublicIncludePaths.Add(Path.Combine(Repo, "source"));
		PublicIncludePaths.Add(Path.Combine(Repo, "extern"));
		// litestl's headers include each other from its root ("math/vector.h"). On Windows that
		// root also answers engine headers' "Math/Vector.h" (CoreUObject's
		// Misc/WorldCompositionUtility.h, under the ITF headers), so litestl's math, and Eigen,
		// arrive outside CoredccIncludesBegin.h, and Eigen's #if tests trip C4668. Search order
		// can't separate the two names; a warning instead of an error is the spike's fix.
		PublicIncludePaths.Add(Path.Combine(Repo, "extern", "litestl"));
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Warning;
		PublicIncludePaths.Add(Path.Combine(Repo, "extern", "litestl", "extern"));
		string Libs = Path.Combine(Repo, "build", "unreal-spike", "libs.txt");
		if (File.Exists(Libs))
		{
			foreach (string Line in File.ReadAllLines(Libs))
			{
				if (Line.Trim().Length > 0) PublicAdditionalLibraries.Add(Line.Trim());
			}
		}
	}
}
