using UnrealBuildTool;
using Microsoft.Extensions.Logging;
using System.Collections.Generic;
using System.IO;

public class MonolithFlow : ModuleRules
{
	public MonolithFlow(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off,
		// so accidentally-bundled binaries can't carry a hard import on Flow DLLs.
		bool bReleaseBuild =
			System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		string FlowUplugin = bReleaseBuild ? null : FindFlowPlugin(Target);
		bool bHasFlow = FlowUplugin != null;

		if (bReleaseBuild)
		{
			Logger.LogInformation("MonolithFlow: MONOLITH_RELEASE_BUILD=1, building the inactive stub");
		}
		else if (bHasFlow)
		{
			Logger.LogInformation("MonolithFlow: Flow plugin found at {Path}", FlowUplugin);
		}
		else
		{
			Logger.LogInformation("MonolithFlow: Flow plugin not found, building the inactive stub (no flow actions)");
		}

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
	/// Locate the MothCocoon Flow plugin: anywhere under the project's Plugins tree
	/// (a GitHub clone lands in "FlowGraph", some projects nest plugins in subfolders),
	/// anywhere under the engine's Plugins/Marketplace (Fab obfuscates folder names),
	/// or at Engine/Plugins/Flow. Matches on the Flow.uplugin file name, which is what
	/// gives the plugin its name - folder-name globs would also hit neighbours like
	/// "FlowValidation/". Returns the descriptor path, or null when Flow is absent.
	/// </summary>
	private static string FindFlowPlugin(ReadOnlyTargetRules Target)
	{
		var Roots = new List<string>();
		if (Target.ProjectFile != null)
		{
			Roots.Add(Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins"));
		}

		string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
		Roots.Add(Path.Combine(EngineDir, "Plugins", "Marketplace"));
		Roots.Add(Path.Combine(EngineDir, "Plugins", "Flow"));

		foreach (string Root in Roots)
		{
			string Found = FindUpluginUnder(Root, "Flow.uplugin");
			if (Found != null)
			{
				return Found;
			}
		}
		return null;
	}

	/// <summary>
	/// Depth-first search for a plugin descriptor. Stops descending at the first
	/// directory that holds any .uplugin (plugins do not nest), and never walks into
	/// a plugin's own Content/Source/Binaries/Intermediate, so a large Plugins tree
	/// costs a handful of directory listings rather than a full file scan.
	/// </summary>
	private static string FindUpluginUnder(string Dir, string UpluginName)
	{
		if (!Directory.Exists(Dir))
		{
			return null;
		}

		string[] Descriptors = Directory.GetFiles(Dir, "*.uplugin", SearchOption.TopDirectoryOnly);
		if (Descriptors.Length > 0)
		{
			foreach (string Descriptor in Descriptors)
			{
				if (string.Equals(Path.GetFileName(Descriptor), UpluginName, System.StringComparison.OrdinalIgnoreCase))
				{
					return Descriptor;
				}
			}
			return null;
		}

		foreach (string Child in Directory.GetDirectories(Dir))
		{
			string Name = Path.GetFileName(Child);
			if (Name.StartsWith(".") || Name == "Binaries" || Name == "Intermediate" || Name == "Saved")
			{
				continue;
			}

			string Found = FindUpluginUnder(Child, UpluginName);
			if (Found != null)
			{
				return Found;
			}
		}
		return null;
	}
}
