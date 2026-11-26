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
		// Only source/ and extern/: litestl includes its own headers, and Eigen, by relative path.
		// With extern/litestl/ on the path, engine headers' "Math/Vector.h" found litestl's
		// math/vector.h (Windows ignores case), so C4668 had to be a warning module-wide.
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
