using UnrealBuildTool;
using System.IO;
using System.Linq;

public class MonolithFlow : ModuleRules
{
	public MonolithFlow(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off,
		// so accidentally-bundled binaries can't carry a hard import on Flow DLLs.
		bool bReleaseBuild =
			System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		bool bHasFlow = !bReleaseBuild && DetectFlowPlugin(Target);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		if (bHasFlow)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MonolithCore",
				"MonolithIndex",
				"SQLiteCore",
				"UnrealEd",
				"AssetRegistry",
				"AssetTools",
				"BlueprintGraph",
				"Kismet",
				"EditorSubsystem",
				"DeveloperSettings",
				"DataValidation",
				"EditorScriptingUtilities",
				"Flow",
				"FlowEditor",
				"GameplayTags",
				"Json",
				"JsonUtilities"
			});
			PublicDefinitions.Add("WITH_FLOW=1");
		}
		else
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MonolithCore",
				"DeveloperSettings"
			});
			PublicDefinitions.Add("WITH_FLOW=0");
		}
	}

	/// <summary>
	/// Probe the three canonical install locations for the MothCocoon Flow plugin.
	/// Matches on Flow.uplugin presence, not folder-name globs - a sibling like
	/// "FlowValidation/" must not count as a hit.
	/// </summary>
	private static bool DetectFlowPlugin(ReadOnlyTargetRules Target)
	{
		static bool HasFlowUplugin(string root) =>
			Directory.Exists(root) && File.Exists(Path.Combine(root, "Flow.uplugin"));

		// 1. Project Plugins/Flow (canonical install location)
		if (Target.ProjectFile != null)
		{
			string projectRoot = Path.Combine(
				Target.ProjectFile.Directory.FullName, "Plugins", "Flow");
			if (HasFlowUplugin(projectRoot))
			{
				return true;
			}
		}

		string engineDir = Path.GetFullPath(Target.RelativeEnginePath);

		// 2. Engine Plugins/Marketplace/ - recursive scan survives Fab folder-name obfuscation
		string marketplaceDir = Path.Combine(engineDir, "Plugins", "Marketplace");
		if (Directory.Exists(marketplaceDir) &&
			Directory.GetFiles(marketplaceDir, "Flow.uplugin", SearchOption.AllDirectories).Any())
		{
			return true;
		}

		// 3. Engine Plugins/Flow (manual engine-side install)
		return HasFlowUplugin(Path.Combine(engineDir, "Plugins", "Flow"));
	}
}
