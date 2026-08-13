// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"

namespace UE::SOMOLMCP
{
namespace EditorBuildPipeline
{
	static bool GetDryRun(const TSharedRef<FJsonObject>& Args)
	{
		bool bDryRun = false;
		Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
		return bDryRun;
	}

	static UWorld* GetEditorWorld(FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available.");
			return nullptr;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			OutError = TEXT("Editor world is not available.");
			return nullptr;
		}
		return World;
	}

	static bool ExecEditorCommand(
		const TCHAR* ToolName,
		const FString& Command,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		Out->SetStringField(TEXT("tool"), ToolName);
		Out->SetStringField(TEXT("command"), Command);
		Out->SetBoolField(TEXT("dry_run"), GetDryRun(Args));

		if (GetDryRun(Args))
		{
			Out->SetStringField(TEXT("status"), TEXT("planned"));
			Summary = FString::Printf(TEXT("%s dry-run planned command: %s"), ToolName, *Command);
			return true;
		}

		UWorld* World = GetEditorWorld(Error);
		if (!World)
		{
			SololmcpError::Set(Out, TEXT("NOT_AVAILABLE"), TEXT(""), Error);
			return false;
		}

		const bool bExecuted = GEditor->Exec(World, *Command);
		Out->SetBoolField(TEXT("executed"), bExecuted);
		Out->SetStringField(TEXT("status"), bExecuted ? TEXT("submitted") : TEXT("rejected"));
		if (!bExecuted)
		{
			Error = FString::Printf(TEXT("Editor rejected command: %s"), *Command);
			SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""), Error);
			return false;
		}
		Summary = FString::Printf(TEXT("%s submitted: %s"), ToolName, *Command);
		return true;
	}

	static FString NormalizeQuality(const FString& In)
	{
		if (In.Equals(TEXT("Medium"), ESearchCase::IgnoreCase)) return TEXT("Medium");
		if (In.Equals(TEXT("High"), ESearchCase::IgnoreCase)) return TEXT("High");
		if (In.Equals(TEXT("Production"), ESearchCase::IgnoreCase)) return TEXT("Production");
		return TEXT("Preview");
	}

	static int32 QualityIndex(const FString& Quality)
	{
		if (Quality == TEXT("Medium")) return 1;
		if (Quality == TEXT("High")) return 2;
		if (Quality == TEXT("Production")) return 3;
		return 0;
	}

	static bool ToolBuildLighting(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		FString Quality;
		Args->TryGetStringField(TEXT("quality"), Quality);
		Quality = NormalizeQuality(Quality);
		const FString Command = FString::Printf(TEXT("BUILDLIGHTING QUALITY=%d SWARM=1"), QualityIndex(Quality));
		Out->SetStringField(TEXT("quality"), Quality);
		Out->SetStringField(TEXT("operation"), TEXT("lighting_build"));
		return ExecEditorCommand(TEXT("editor_build_lighting"), Command, Args, Out, Summary, Error);
	}

	static bool ToolBuildNavigation(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		Out->SetStringField(TEXT("operation"), TEXT("navigation_build"));
		return ExecEditorCommand(TEXT("editor_build_navigation"), TEXT("REBUILDNAV"), Args, Out, Summary, Error);
	}

	static bool ToolBuildReflectionCaptures(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		Out->SetStringField(TEXT("operation"), TEXT("reflection_capture_build"));
		return ExecEditorCommand(TEXT("editor_build_reflection_captures"), TEXT("BUILDREFLECTIONCAPTURES"), Args, Out, Summary, Error);
	}

	static bool ToolBuildAiData(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		Out->SetStringField(TEXT("operation"), TEXT("ai_data_build"));
		Out->SetStringField(TEXT("note"), TEXT("Builds editor navigation data used by AI agents. BehaviorTree/StateTree assets still compile through their dedicated asset tools."));
		return ExecEditorCommand(TEXT("editor_build_ai_data"), TEXT("REBUILDNAV"), Args, Out, Summary, Error);
	}

	static bool ToolCompileShaders(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		FString Scope = TEXT("Changed");
		Args->TryGetStringField(TEXT("scope"), Scope);
		if (!Scope.Equals(TEXT("All"), ESearchCase::IgnoreCase)
			&& !Scope.Equals(TEXT("Global"), ESearchCase::IgnoreCase)
			&& !Scope.Equals(TEXT("Material"), ESearchCase::IgnoreCase))
		{
			Scope = TEXT("Changed");
		}
		FString Command = TEXT("RECOMPILESHADERS CHANGED");
		if (Scope.Equals(TEXT("All"), ESearchCase::IgnoreCase)) Command = TEXT("RECOMPILESHADERS ALL");
		else if (Scope.Equals(TEXT("Global"), ESearchCase::IgnoreCase)) Command = TEXT("RECOMPILESHADERS GLOBAL");
		else if (Scope.Equals(TEXT("Material"), ESearchCase::IgnoreCase)) Command = TEXT("RECOMPILESHADERS CHANGED");
		Out->SetStringField(TEXT("scope"), Scope);
		Out->SetStringField(TEXT("operation"), TEXT("shader_compile"));
		return ExecEditorCommand(TEXT("editor_compile_shaders"), Command, Args, Out, Summary, Error);
	}

	static void SetStringArray(TSharedRef<FJsonObject> Out, const FString& Field, const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		Out->SetArrayField(Field, Values);
	}

	static void ReadStringArray(const TSharedRef<FJsonObject>& Args, const FString& Field, TArray<FString>& OutItems)
	{
		const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
		if (!Args->TryGetArrayField(Field, Raw) || !Raw)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Raw)
		{
			FString S;
			if (Value.IsValid() && Value->TryGetString(S) && !S.IsEmpty())
			{
				OutItems.Add(S);
			}
		}
	}

	static TSharedPtr<FJsonValue> ConfigStringToJsonValue(const FString& Value)
	{
		if (Value.Equals(TEXT("True"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("False"), ESearchCase::IgnoreCase))
		{
			return MakeShared<FJsonValueBoolean>(Value.ToBool());
		}

		if (Value.IsNumeric())
		{
			if (Value.Contains(TEXT(".")))
			{
				return MakeShared<FJsonValueNumber>(FCString::Atod(*Value));
			}
			return MakeShared<FJsonValueNumber>(FCString::Atoi64(*Value));
		}

		return MakeShared<FJsonValueString>(Value);
	}

	static void SetConfigValueFromJson(
		const TCHAR* Section,
		const FString& Key,
		const TSharedPtr<FJsonValue>& Value,
		const FString& Ini,
		TArray<FString>& Changed)
	{
		if (!Value.IsValid() || !GConfig)
		{
			return;
		}

		switch (Value->Type)
		{
		case EJson::Boolean:
			GConfig->SetBool(Section, *Key, Value->AsBool(), Ini);
			Changed.Add(Key);
			break;
		case EJson::Number:
			GConfig->SetString(Section, *Key, *FString::SanitizeFloat(Value->AsNumber()), Ini);
			Changed.Add(Key);
			break;
		case EJson::String:
			GConfig->SetString(Section, *Key, *Value->AsString(), Ini);
			Changed.Add(Key);
			break;
		case EJson::Array:
		{
			TArray<FString> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (!Item.IsValid())
				{
					continue;
				}
				if (Item->Type == EJson::String)
				{
					Items.Add(Item->AsString());
				}
				else if (Item->Type == EJson::Number)
				{
					Items.Add(FString::SanitizeFloat(Item->AsNumber()));
				}
				else if (Item->Type == EJson::Boolean)
				{
					Items.Add(Item->AsBool() ? TEXT("True") : TEXT("False"));
				}
			}
			GConfig->SetArray(Section, *Key, Items, Ini);
			Changed.Add(Key);
			break;
		}
		default:
			break;
		}
	}

	static bool SetConfigFileValueFromJson(
		FConfigFile& ConfigFile,
		const TCHAR* Section,
		const FString& Key,
		const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		switch (Value->Type)
		{
		case EJson::Boolean:
			ConfigFile.SetBool(Section, *Key, Value->AsBool());
			return true;
		case EJson::Number:
			ConfigFile.SetString(Section, *Key, *FString::SanitizeFloat(Value->AsNumber()));
			return true;
		case EJson::String:
			ConfigFile.SetString(Section, *Key, *Value->AsString());
			return true;
		case EJson::Array:
		{
			TArray<FString> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (!Item.IsValid()) continue;
				if (Item->Type == EJson::String) Items.Add(Item->AsString());
				else if (Item->Type == EJson::Number) Items.Add(FString::SanitizeFloat(Item->AsNumber()));
				else if (Item->Type == EJson::Boolean) Items.Add(Item->AsBool() ? TEXT("True") : TEXT("False"));
			}
			ConfigFile.SetArray(Section, *Key, Items);
			return true;
		}
		default:
			return false;
		}
	}

	static bool ConfigFileValueMatchesJson(
		const FConfigFile& ConfigFile,
		const TCHAR* Section,
		const FString& Key,
		const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) return false;
		if (Value->Type == EJson::Array)
		{
			TArray<FString> Actual;
			ConfigFile.GetArray(Section, *Key, Actual);
			const TArray<TSharedPtr<FJsonValue>>& ExpectedValues = Value->AsArray();
			if (Actual.Num() != ExpectedValues.Num()) return false;
			for (int32 Index = 0; Index < Actual.Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& ExpectedValue = ExpectedValues[Index];
				FString Expected;
				if (!ExpectedValue.IsValid()) return false;
				if (ExpectedValue->Type == EJson::String) Expected = ExpectedValue->AsString();
				else if (ExpectedValue->Type == EJson::Number) Expected = FString::SanitizeFloat(ExpectedValue->AsNumber());
				else if (ExpectedValue->Type == EJson::Boolean) Expected = ExpectedValue->AsBool() ? TEXT("True") : TEXT("False");
				else return false;
				if (Actual[Index] != Expected) return false;
			}
			return true;
		}

		FString Actual;
		if (!ConfigFile.GetString(Section, *Key, Actual)) return false;
		if (Value->Type == EJson::Boolean)
		{
			return Actual.Equals(Value->AsBool() ? TEXT("True") : TEXT("False"), ESearchCase::IgnoreCase);
		}
		if (Value->Type == EJson::Number)
		{
			return Actual.IsNumeric() && FMath::IsNearlyEqual(FCString::Atod(*Actual), Value->AsNumber());
		}
		return Value->Type == EJson::String && Actual == Value->AsString();
	}

	static void AddConfigKeySnapshot(
		const TCHAR* Section,
		const FString& Key,
		const FString& Ini,
		TSharedRef<FJsonObject> OutObject)
	{
		if (!GConfig)
		{
			return;
		}
		TArray<FString> ArrayValues;
		if (GConfig->GetArray(Section, *Key, ArrayValues, Ini) && ArrayValues.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> JsonArray;
			for (const FString& Item : ArrayValues)
			{
				JsonArray.Add(MakeShared<FJsonValueString>(Item));
			}
			OutObject->SetArrayField(Key, JsonArray);
			return;
		}
		FString Raw;
		if (GConfig->GetString(Section, *Key, Raw, Ini))
		{
			OutObject->SetField(Key, ConfigStringToJsonValue(Raw));
		}
	}

	static TArray<FString> KnownPackagingKeys()
	{
		return {
			TEXT("BuildConfiguration"),
			TEXT("BuildTarget"),
			TEXT("StagingDirectory"),
			TEXT("FullRebuild"),
			TEXT("ForDistribution"),
			TEXT("IncludeDebugFiles"),
			TEXT("IncludePrerequisites"),
			TEXT("IncludeAppLocalPrerequisites"),
			TEXT("ApplocalPrerequisitesDirectory"),
			TEXT("UsePakFile"),
			TEXT("bUseIoStore"),
			TEXT("bUseZenStore"),
			TEXT("bMakeBinaryConfig"),
			TEXT("bGenerateChunks"),
			TEXT("bGenerateNoChunks"),
			TEXT("bChunkHardReferencesOnly"),
			TEXT("bForceOneChunkPerFile"),
			TEXT("MaxChunkSize"),
			TEXT("bShareMaterialShaderCode"),
			TEXT("bSharedMaterialNativeLibraries"),
			TEXT("bCompressed"),
			TEXT("PackageCompressionFormat"),
			TEXT("PackageCompressionMethod"),
			TEXT("PackageCompressionLevel_DebugDevelopment"),
			TEXT("PackageCompressionLevel_TestShipping"),
			TEXT("PackageAdditionalCompressionOptions"),
			TEXT("bPackageCompressionEnableDDC"),
			TEXT("PackageCompressionMinBytesSaved"),
			TEXT("PackageCompressionMinPercentSaved"),
			TEXT("DirectoriesToAlwaysCook"),
			TEXT("DirectoriesToNeverCook"),
			TEXT("DirectoriesToAlwaysStageAsUFS"),
			TEXT("DirectoriesToAlwaysStageAsNonUFS"),
			TEXT("DirectoriesToAlwaysStageAsUFSServer"),
			TEXT("DirectoriesToAlwaysStageAsNonUFSServer"),
			TEXT("MapsToCook"),
			TEXT("bCookAll"),
			TEXT("bCookMapsOnly"),
			TEXT("bSkipEditorContent"),
			TEXT("bSkipMovies"),
			TEXT("IniKeyBlacklist"),
			TEXT("IniSectionBlacklist"),
			TEXT("TestDirectoriesToNotSearch")
		};
	}

	static FString PlatformSettingsSection(const FString& Platform)
	{
		if (Platform.Equals(TEXT("Win64"), ESearchCase::IgnoreCase) || Platform.Equals(TEXT("Windows"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Script/WindowsTargetPlatform.WindowsTargetSettings");
		}
		if (Platform.Equals(TEXT("Linux"), ESearchCase::IgnoreCase) || Platform.Equals(TEXT("LinuxArm64"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Script/LinuxTargetPlatform.LinuxTargetSettings");
		}
		if (Platform.Equals(TEXT("Android"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings");
		}
		if (Platform.Equals(TEXT("IOS"), ESearchCase::IgnoreCase) || Platform.Equals(TEXT("iOS"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Script/IOSRuntimeSettings.IOSRuntimeSettings");
		}
		if (Platform.Equals(TEXT("Mac"), ESearchCase::IgnoreCase) || Platform.Equals(TEXT("MacOS"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Script/MacTargetPlatform.MacTargetSettings");
		}
		return FString();
	}

	static bool ToolPackagingGet(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>&,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		if (!GConfig)
		{
			Error = TEXT("GConfig is not available.");
			return false;
		}
		const TCHAR* Section = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		const FString Ini = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
		FString BuildConfiguration, StagingDirectory;
		bool bUsePakFile = false;
		bool bUseIoStore = false;
		bool bFullRebuild = false;
		bool bForDistribution = false;
		bool bIncludeDebugFiles = false;
		bool bIncludePrerequisites = false;
		bool bCookAll = false;
		GConfig->GetString(Section, TEXT("BuildConfiguration"), BuildConfiguration, Ini);
		GConfig->GetString(Section, TEXT("StagingDirectory"), StagingDirectory, Ini);
		GConfig->GetBool(Section, TEXT("UsePakFile"), bUsePakFile, Ini);
		GConfig->GetBool(Section, TEXT("bUseIoStore"), bUseIoStore, Ini);
		GConfig->GetBool(Section, TEXT("FullRebuild"), bFullRebuild, Ini);
		GConfig->GetBool(Section, TEXT("ForDistribution"), bForDistribution, Ini);
		GConfig->GetBool(Section, TEXT("IncludeDebugFiles"), bIncludeDebugFiles, Ini);
		GConfig->GetBool(Section, TEXT("IncludePrerequisites"), bIncludePrerequisites, Ini);
		GConfig->GetBool(Section, TEXT("bCookAll"), bCookAll, Ini);
		Out->SetStringField(TEXT("config_file"), Ini);
		Out->SetStringField(TEXT("build_configuration"), BuildConfiguration);
		Out->SetStringField(TEXT("staging_directory"), StagingDirectory);
		Out->SetBoolField(TEXT("use_pak"), bUsePakFile);
		Out->SetBoolField(TEXT("use_iostore"), bUseIoStore);
		Out->SetBoolField(TEXT("full_rebuild"), bFullRebuild);
		Out->SetBoolField(TEXT("for_distribution"), bForDistribution);
		Out->SetBoolField(TEXT("include_debug_files"), bIncludeDebugFiles);
		Out->SetBoolField(TEXT("include_prerequisites"), bIncludePrerequisites);
		Out->SetBoolField(TEXT("cook_all"), bCookAll);
		Summary = TEXT("Read ProjectPackagingSettings from DefaultGame.ini.");
		return true;
	}

	static bool ToolPackagingSet(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		if (!GConfig)
		{
			Error = TEXT("GConfig is not available.");
			return false;
		}
		const TCHAR* Section = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		const FString Ini = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
		const bool bDryRun = GetDryRun(Args);
		TArray<FString> Changed;

		auto SetStringIfPresent = [&](const TCHAR* JsonKey, const TCHAR* IniKey)
		{
			FString Value;
			if (Args->TryGetStringField(JsonKey, Value))
			{
				Changed.Add(IniKey);
				if (!bDryRun) GConfig->SetString(Section, IniKey, *Value, Ini);
			}
		};
		auto SetBoolIfPresent = [&](const TCHAR* JsonKey, const TCHAR* IniKey)
		{
			bool Value = false;
			if (Args->TryGetBoolField(JsonKey, Value))
			{
				Changed.Add(IniKey);
				if (!bDryRun) GConfig->SetBool(Section, IniKey, Value, Ini);
			}
		};

		SetStringIfPresent(TEXT("build_configuration"), TEXT("BuildConfiguration"));
		SetStringIfPresent(TEXT("staging_directory"), TEXT("StagingDirectory"));
		SetBoolIfPresent(TEXT("use_pak"), TEXT("UsePakFile"));
		SetBoolIfPresent(TEXT("use_iostore"), TEXT("bUseIoStore"));
		SetBoolIfPresent(TEXT("full_rebuild"), TEXT("FullRebuild"));
		SetBoolIfPresent(TEXT("for_distribution"), TEXT("ForDistribution"));
		SetBoolIfPresent(TEXT("include_debug_files"), TEXT("IncludeDebugFiles"));
		SetBoolIfPresent(TEXT("include_prerequisites"), TEXT("IncludePrerequisites"));
		SetBoolIfPresent(TEXT("cook_all"), TEXT("bCookAll"));

		TArray<FString> MapsToCook;
		ReadStringArray(Args, TEXT("maps_to_cook"), MapsToCook);
		if (!MapsToCook.IsEmpty())
		{
			Changed.Add(TEXT("MapsToCook"));
			Out->SetStringField(TEXT("maps_to_cook_note"), TEXT("maps_to_cook is acknowledged in the receipt; array replacement is intentionally left to package_build additional_args to avoid clobbering unrelated ProjectPackagingSettings keys."));
		}

		if (!bDryRun)
		{
			GConfig->Flush(false, Ini);
		}
		Out->SetStringField(TEXT("config_file"), Ini);
		Out->SetBoolField(TEXT("dry_run"), bDryRun);
		SetStringArray(Out, TEXT("changed_keys"), Changed);
		Out->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("written"));
		Summary = FString::Printf(TEXT("Packaging profile %s (%d changed keys)."), bDryRun ? TEXT("planned") : TEXT("written"), Changed.Num());
		return true;
	}

	static bool ToolPackagingSettingsExportFull(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		if (!GConfig)
		{
			Error = TEXT("GConfig is not available.");
			return false;
		}

		const FString EngineVersion = FEngineVersion::Current().ToString();
		const TCHAR* PackagingSection = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		const FString GameIni = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
		const FString EngineIni = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini"));

		TSharedRef<FJsonObject> Packaging = MakeShared<FJsonObject>();
		TArray<FString> Keys = KnownPackagingKeys();
		TArray<FString> RequestedKeys;
		ReadStringArray(Args, TEXT("extra_keys"), RequestedKeys);
		for (const FString& Key : RequestedKeys)
		{
			Keys.AddUnique(Key);
		}
		for (const FString& Key : Keys)
		{
			AddConfigKeySnapshot(PackagingSection, Key, GameIni, Packaging);
		}

		TSharedRef<FJsonObject> PlatformSections = MakeShared<FJsonObject>();
		TArray<FString> Platforms;
		ReadStringArray(Args, TEXT("platforms"), Platforms);
		if (Platforms.IsEmpty())
		{
			Platforms = {TEXT("Win64"), TEXT("Linux"), TEXT("Android"), TEXT("IOS"), TEXT("Mac")};
		}
		for (const FString& Platform : Platforms)
		{
			const FString Section = PlatformSettingsSection(Platform);
			if (Section.IsEmpty())
			{
				continue;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("section"), Section);
			Entry->SetStringField(TEXT("config_file"), EngineIni);
			PlatformSections->SetObjectField(Platform, Entry);
		}

		Out->SetStringField(TEXT("engine_version"), EngineVersion);
		Out->SetStringField(TEXT("project_dir"), FPaths::ProjectDir());
		Out->SetStringField(TEXT("packaging_section"), PackagingSection);
		Out->SetStringField(TEXT("packaging_config_file"), GameIni);
		Out->SetStringField(TEXT("platform_config_file"), EngineIni);
		Out->SetObjectField(TEXT("packaging_settings"), Packaging);
		Out->SetObjectField(TEXT("platform_sections"), PlatformSections);
		Out->SetArrayField(TEXT("known_packaging_keys"), [&Keys]()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Key : Keys) Values.Add(MakeShared<FJsonValueString>(Key));
			return Values;
		}());
		Summary = FString::Printf(TEXT("Exported ProjectPackagingSettings snapshot for UE %s."), *EngineVersion);
		return true;
	}

	static bool ToolPackagingSettingsPatchFull(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		if (!GConfig)
		{
			Error = TEXT("GConfig is not available.");
			return false;
		}

		const bool bDryRun = GetDryRun(Args);
		const FString GameIni = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
		const FString EngineIni = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini"));
		const TCHAR* PackagingSection = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		TArray<FString> Changed;

		const TSharedPtr<FJsonObject>* PackagingPatch = nullptr;
		if (Args->TryGetObjectField(TEXT("packaging_settings"), PackagingPatch) && PackagingPatch && PackagingPatch->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PackagingPatch)->Values)
			{
				if (!bDryRun)
				{
					SetConfigValueFromJson(PackagingSection, Pair.Key, Pair.Value, GameIni, Changed);
				}
				else if (Pair.Value.IsValid())
				{
					Changed.Add(Pair.Key);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* PlatformPatches = nullptr;
		TArray<TSharedPtr<FJsonValue>> PlatformResults;
		if (Args->TryGetArrayField(TEXT("platform_settings"), PlatformPatches) && PlatformPatches)
		{
			for (const TSharedPtr<FJsonValue>& RowValue : *PlatformPatches)
			{
				const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
				if (!Row.IsValid())
				{
					continue;
				}
				FString Platform;
				Row->TryGetStringField(TEXT("platform"), Platform);
				FString Section;
				Row->TryGetStringField(TEXT("section"), Section);
				if (Section.IsEmpty())
				{
					Section = PlatformSettingsSection(Platform);
				}
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("platform"), Platform);
				Result->SetStringField(TEXT("section"), Section);
				if (Section.IsEmpty())
				{
					Result->SetStringField(TEXT("status"), TEXT("skipped_unknown_platform_section"));
					PlatformResults.Add(MakeShared<FJsonValueObject>(Result));
					continue;
				}
				const TSharedPtr<FJsonObject>* Settings = nullptr;
				if (Row->TryGetObjectField(TEXT("settings"), Settings) && Settings && Settings->IsValid())
				{
					int32 Before = Changed.Num();
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Settings)->Values)
					{
						if (!bDryRun)
						{
							SetConfigValueFromJson(*Section, Pair.Key, Pair.Value, EngineIni, Changed);
						}
						else if (Pair.Value.IsValid())
						{
							Changed.Add(Section + TEXT(".") + Pair.Key);
						}
					}
					Result->SetNumberField(TEXT("changed_key_count"), Changed.Num() - Before);
					Result->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("written"));
				}
				PlatformResults.Add(MakeShared<FJsonValueObject>(Result));
			}
		}

		if (!bDryRun)
		{
			GConfig->Flush(false, GameIni);
			GConfig->Flush(false, EngineIni);
		}
		Out->SetBoolField(TEXT("dry_run"), bDryRun);
		Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Out->SetStringField(TEXT("packaging_config_file"), GameIni);
		Out->SetStringField(TEXT("platform_config_file"), EngineIni);
		SetStringArray(Out, TEXT("changed_keys"), Changed);
		Out->SetArrayField(TEXT("platform_results"), PlatformResults);
		Out->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("written"));
		Summary = FString::Printf(TEXT("Packaging/platform settings %s (%d keys)."), bDryRun ? TEXT("planned") : TEXT("written"), Changed.Num());
		return true;
	}

	static bool ToolPackagingPlatformProfileApply(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		FString Platform;
		if (!Args->TryGetStringField(TEXT("target_platform"), Platform) || Platform.IsEmpty())
		{
			Error = TEXT("target_platform is required.");
			return false;
		}

		const bool bDryRun = GetDryRun(Args);
		const FString GameIni = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
		const FString EngineIni = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini"));
		const TCHAR* PackagingSection = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		TArray<FString> Changed;

		FString BuildConfiguration = TEXT("Development");
		Args->TryGetStringField(TEXT("build_configuration"), BuildConfiguration);
		bool bShippingLike = BuildConfiguration.Equals(TEXT("Shipping"), ESearchCase::IgnoreCase)
			|| BuildConfiguration.Equals(TEXT("Test"), ESearchCase::IgnoreCase);

		bool bUsePak = true;
		bool bUseIoStore = true;
		bool bCompressed = bShippingLike;
		bool bForDistribution = bShippingLike;
		bool bFullRebuild = true;
		bool bSkipEditorContent = true;
		Args->TryGetBoolField(TEXT("use_pak"), bUsePak);
		Args->TryGetBoolField(TEXT("use_iostore"), bUseIoStore);
		Args->TryGetBoolField(TEXT("compressed"), bCompressed);
		Args->TryGetBoolField(TEXT("for_distribution"), bForDistribution);
		Args->TryGetBoolField(TEXT("full_rebuild"), bFullRebuild);
		Args->TryGetBoolField(TEXT("skip_editor_content"), bSkipEditorContent);

		auto WriteBool = [&](const TCHAR* Key, bool Value)
		{
			if (!bDryRun)
			{
				GConfig->SetBool(PackagingSection, Key, Value, GameIni);
			}
			Changed.Add(Key);
		};
		auto WriteString = [&](const TCHAR* Key, const FString& Value)
		{
			if (!bDryRun)
			{
				GConfig->SetString(PackagingSection, Key, *Value, GameIni);
			}
			Changed.Add(Key);
		};

		WriteString(TEXT("BuildConfiguration"), BuildConfiguration);
		WriteBool(TEXT("UsePakFile"), bUsePak);
		WriteBool(TEXT("bUseIoStore"), bUseIoStore);
		WriteBool(TEXT("bCompressed"), bCompressed);
		WriteBool(TEXT("ForDistribution"), bForDistribution);
		WriteBool(TEXT("FullRebuild"), bFullRebuild);
		WriteBool(TEXT("bSkipEditorContent"), bSkipEditorContent);

		FString StagingDirectory;
		if (Args->TryGetStringField(TEXT("staging_directory"), StagingDirectory) && !StagingDirectory.IsEmpty())
		{
			WriteString(TEXT("StagingDirectory"), StagingDirectory);
		}

		TArray<FString> MapsToCook;
		ReadStringArray(Args, TEXT("maps_to_cook"), MapsToCook);
		if (!MapsToCook.IsEmpty())
		{
			if (!bDryRun)
			{
				GConfig->SetArray(PackagingSection, TEXT("MapsToCook"), MapsToCook, GameIni);
			}
			Changed.Add(TEXT("MapsToCook"));
		}

		const TSharedPtr<FJsonObject>* AdditionalPackaging = nullptr;
		if (Args->TryGetObjectField(TEXT("additional_packaging_settings"), AdditionalPackaging) && AdditionalPackaging && AdditionalPackaging->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*AdditionalPackaging)->Values)
			{
				if (!bDryRun)
				{
					SetConfigValueFromJson(PackagingSection, Pair.Key, Pair.Value, GameIni, Changed);
				}
				else
				{
					Changed.Add(Pair.Key);
				}
			}
		}

		const FString PlatformSection = PlatformSettingsSection(Platform);
		const TSharedPtr<FJsonObject>* PlatformSettings = nullptr;
		if (!PlatformSection.IsEmpty() && Args->TryGetObjectField(TEXT("platform_settings"), PlatformSettings) && PlatformSettings && PlatformSettings->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PlatformSettings)->Values)
			{
				if (!bDryRun)
				{
					SetConfigValueFromJson(*PlatformSection, Pair.Key, Pair.Value, EngineIni, Changed);
				}
				else
				{
					Changed.Add(PlatformSection + TEXT(".") + Pair.Key);
				}
			}
		}

		if (!bDryRun)
		{
			GConfig->Flush(false, GameIni);
			GConfig->Flush(false, EngineIni);
		}

		TSharedRef<FJsonObject> PackageBuildArgs = MakeShared<FJsonObject>();
		PackageBuildArgs->SetStringField(TEXT("target_platform"), Platform);
		PackageBuildArgs->SetStringField(TEXT("configuration"), BuildConfiguration);
		if (!StagingDirectory.IsEmpty())
		{
			PackageBuildArgs->SetStringField(TEXT("archive_directory"), StagingDirectory);
		}

		Out->SetBoolField(TEXT("dry_run"), bDryRun);
		Out->SetStringField(TEXT("target_platform"), Platform);
		Out->SetStringField(TEXT("platform_settings_section"), PlatformSection);
		Out->SetStringField(TEXT("packaging_config_file"), GameIni);
		Out->SetStringField(TEXT("platform_config_file"), EngineIni);
		SetStringArray(Out, TEXT("changed_keys"), Changed);
		Out->SetObjectField(TEXT("package_build_arguments"), PackageBuildArgs);
		Out->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("written"));
		Summary = FString::Printf(TEXT("Applied %s packaging profile for %s (%d keys)."), *BuildConfiguration, *Platform, Changed.Num());
		return true;
	}

	static bool ToolPackagingSettingsValidate(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		if (!GConfig)
		{
			Error = TEXT("GConfig is not available.");
			return false;
		}
		FString Platform;
		Args->TryGetStringField(TEXT("target_platform"), Platform);
		const FString GameIni = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
		const TCHAR* PackagingSection = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		TArray<TSharedPtr<FJsonValue>> Checks;
		int32 Passed = 0;
		int32 Failed = 0;
		auto AddCheck = [&](const FString& Name, bool bPass, const FString& Detail)
		{
			TSharedRef<FJsonObject> Check = MakeShared<FJsonObject>();
			Check->SetStringField(TEXT("name"), Name);
			Check->SetBoolField(TEXT("pass"), bPass);
			Check->SetStringField(TEXT("detail"), Detail);
			Checks.Add(MakeShared<FJsonValueObject>(Check));
			if (bPass) ++Passed; else ++Failed;
		};

		FString BuildConfiguration;
		GConfig->GetString(PackagingSection, TEXT("BuildConfiguration"), BuildConfiguration, GameIni);
		bool bUsePak = false;
		bool bUseIoStore = false;
		bool bForDistribution = false;
		GConfig->GetBool(PackagingSection, TEXT("UsePakFile"), bUsePak, GameIni);
		GConfig->GetBool(PackagingSection, TEXT("bUseIoStore"), bUseIoStore, GameIni);
		GConfig->GetBool(PackagingSection, TEXT("ForDistribution"), bForDistribution, GameIni);
		TArray<FString> MapsToCook;
		GConfig->GetArray(PackagingSection, TEXT("MapsToCook"), MapsToCook, GameIni);

		AddCheck(TEXT("build_configuration_present"), !BuildConfiguration.IsEmpty(), BuildConfiguration);
		AddCheck(TEXT("pak_or_iostore_enabled"), bUsePak || bUseIoStore, FString::Printf(TEXT("UsePakFile=%s bUseIoStore=%s"), bUsePak ? TEXT("true") : TEXT("false"), bUseIoStore ? TEXT("true") : TEXT("false")));
		if (BuildConfiguration.Equals(TEXT("Shipping"), ESearchCase::IgnoreCase))
		{
			AddCheck(TEXT("shipping_distribution_consistency"), bForDistribution, TEXT("Shipping should normally set ForDistribution=true for store/package builds."));
		}
		if (!Platform.IsEmpty())
		{
			AddCheck(TEXT("platform_known"), !PlatformSettingsSection(Platform).IsEmpty(), Platform);
		}
		AddCheck(TEXT("maps_to_cook_known"), MapsToCook.Num() > 0, MapsToCook.Num() > 0 ? FString::Printf(TEXT("%d maps configured."), MapsToCook.Num()) : TEXT("No MapsToCook configured; UAT may cook defaults or require explicit additional_args."));

		Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Out->SetStringField(TEXT("target_platform"), Platform);
		Out->SetStringField(TEXT("config_file"), GameIni);
		Out->SetArrayField(TEXT("checks"), Checks);
		Out->SetNumberField(TEXT("passed"), Passed);
		Out->SetNumberField(TEXT("failed"), Failed);
		Out->SetBoolField(TEXT("valid"), Failed == 0);
		Out->SetStringField(TEXT("status"), Failed == 0 ? TEXT("passed") : TEXT("failed"));
		Summary = FString::Printf(TEXT("Packaging settings validation: %d passed, %d failed."), Passed, Failed);
		return Failed == 0;
	}

	static bool ResolveProjectConfigFile(const FString& ConfigName, FString& OutPath, FString& OutError)
	{
		static const TSet<FString> Allowed = {
			TEXT("DefaultEngine.ini"), TEXT("DefaultGame.ini"), TEXT("DefaultInput.ini"),
			TEXT("DefaultEditor.ini"), TEXT("DefaultEditorPerProjectUserSettings.ini")
		};
		FString CleanName = FPaths::GetCleanFilename(ConfigName);
		if (!Allowed.Contains(CleanName))
		{
			OutError = FString::Printf(TEXT("Unsupported project config file '%s'."), *ConfigName);
			return false;
		}
		OutPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / CleanName);
		return true;
	}

	static bool ToolProjectSettingsExport(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		FString ConfigName = TEXT("DefaultEngine.ini");
		Args->TryGetStringField(TEXT("config_file"), ConfigName);
		FString ConfigPath;
		if (!ResolveProjectConfigFile(ConfigName, ConfigPath, Error) || !GConfig)
		{
			if (Error.IsEmpty()) Error = TEXT("GConfig is not available.");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Requests = nullptr;
		if (!Args->TryGetArrayField(TEXT("requests"), Requests) || !Requests)
		{
			Error = TEXT("requests is required.");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Results;
		for (const TSharedPtr<FJsonValue>& RequestValue : *Requests)
		{
			const TSharedPtr<FJsonObject> Request = RequestValue.IsValid() ? RequestValue->AsObject() : nullptr;
			if (!Request.IsValid()) continue;
			FString Section;
			Request->TryGetStringField(TEXT("section"), Section);
			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			Request->TryGetArrayField(TEXT("keys"), Keys);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("section"), Section);
			TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
			if (!Section.IsEmpty() && Keys)
			{
				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
				{
					FString Key;
					if (!KeyValue.IsValid() || !KeyValue->TryGetString(Key) || Key.IsEmpty()) continue;
					FString Value;
					if (GConfig->GetString(*Section, *Key, Value, ConfigPath)) Values->SetStringField(Key, Value);
					else Values->SetField(Key, MakeShared<FJsonValueNull>());
				}
			}
			Result->SetObjectField(TEXT("values"), Values);
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
		Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Out->SetStringField(TEXT("config_file"), ConfigPath);
		Out->SetArrayField(TEXT("results"), Results);
		Out->SetStringField(TEXT("status"), TEXT("read"));
		Summary = FString::Printf(TEXT("Exported %d project-settings sections from %s."), Results.Num(), *ConfigName);
		return true;
	}

	static bool ToolProjectSettingsPatch(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		FString ConfigName = TEXT("DefaultEngine.ini");
		Args->TryGetStringField(TEXT("config_file"), ConfigName);
		FString ConfigPath;
		if (!ResolveProjectConfigFile(ConfigName, ConfigPath, Error) || !GConfig)
		{
			if (Error.IsEmpty()) Error = TEXT("GConfig is not available.");
			return false;
		}
		const bool bDryRun = GetDryRun(Args);
		const TArray<TSharedPtr<FJsonValue>>* Patches = nullptr;
		if (!Args->TryGetArrayField(TEXT("patches"), Patches) || !Patches)
		{
			Error = TEXT("patches is required.");
			return false;
		}
		TArray<FString> Changed;
		FConfigFile ConfigFile;
		if (!bDryRun)
		{
			ConfigFile.Read(ConfigPath);
			ConfigFile.NoSave = false;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
			// FConfigFile gained bCanSaveAllSections in 5.4. On 5.3 the writer already
			// saves every section it was given, so there is nothing to opt into and
			// nothing is lost by omitting it.
			ConfigFile.bCanSaveAllSections = true;
#endif
		}
		for (const TSharedPtr<FJsonValue>& PatchValue : *Patches)
		{
			const TSharedPtr<FJsonObject> Patch = PatchValue.IsValid() ? PatchValue->AsObject() : nullptr;
			if (!Patch.IsValid()) continue;
			FString Section;
			Patch->TryGetStringField(TEXT("section"), Section);
			const TSharedPtr<FJsonObject>* Settings = nullptr;
			if (Section.IsEmpty() || !Patch->TryGetObjectField(TEXT("settings"), Settings) || !Settings || !Settings->IsValid()) continue;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Settings)->Values)
			{
				if (!bDryRun)
				{
					if (SetConfigFileValueFromJson(ConfigFile, *Section, Pair.Key, Pair.Value))
					{
						Changed.Add(Section + TEXT(".") + Pair.Key);
					}
				}
				else Changed.Add(Section + TEXT(".") + Pair.Key);
			}
		}
		bool bWriteVerified = bDryRun;
		if (!bDryRun)
		{
			if (!ConfigFile.Write(ConfigPath, false))
			{
				Error = FString::Printf(TEXT("Failed to write project config file: %s"), *ConfigPath);
				return false;
			}
			FConfigFile ReadBack;
			ReadBack.Read(ConfigPath);
			bWriteVerified = true;
			for (const TSharedPtr<FJsonValue>& PatchValue : *Patches)
			{
				const TSharedPtr<FJsonObject> Patch = PatchValue.IsValid() ? PatchValue->AsObject() : nullptr;
				if (!Patch.IsValid()) continue;
				FString Section;
				Patch->TryGetStringField(TEXT("section"), Section);
				const TSharedPtr<FJsonObject>* Settings = nullptr;
				if (Section.IsEmpty() || !Patch->TryGetObjectField(TEXT("settings"), Settings) || !Settings || !Settings->IsValid()) continue;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Settings)->Values)
				{
					if (!ConfigFileValueMatchesJson(ReadBack, *Section, Pair.Key, Pair.Value))
					{
						bWriteVerified = false;
						Error = FString::Printf(TEXT("Config write readback mismatch: [%s] %s"), *Section, *Pair.Key);
						break;
					}
				}
				if (!bWriteVerified) break;
			}
			if (!bWriteVerified) return false;
		}
		Out->SetBoolField(TEXT("dry_run"), bDryRun);
		Out->SetBoolField(TEXT("write_verified"), bWriteVerified);
		Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Out->SetStringField(TEXT("config_file"), ConfigPath);
		SetStringArray(Out, TEXT("changed_keys"), Changed);
		Out->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("written"));
		Summary = FString::Printf(TEXT("Project settings %s in %s (%d keys)."), bDryRun ? TEXT("planned") : TEXT("written"), *ConfigName, Changed.Num());
		return true;
	}

	static bool ToolBuildPipelinePlan(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString&)
	{
		bool bIncludeLighting = true;
		bool bIncludeNavigation = true;
		bool bIncludeShaders = true;
		bool bIncludePackage = true;
		Args->TryGetBoolField(TEXT("include_lighting"), bIncludeLighting);
		Args->TryGetBoolField(TEXT("include_navigation"), bIncludeNavigation);
		Args->TryGetBoolField(TEXT("include_shaders"), bIncludeShaders);
		Args->TryGetBoolField(TEXT("include_package"), bIncludePackage);

		TArray<TSharedPtr<FJsonValue>> Steps;
		auto AddStep = [&](const FString& Tool, const TSharedRef<FJsonObject>& Arguments)
		{
			TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("tool"), Tool);
			Step->SetObjectField(TEXT("arguments"), Arguments);
			Steps.Add(MakeShared<FJsonValueObject>(Step));
		};
		if (bIncludeShaders) AddStep(TEXT("editor_compile_shaders"), MakeShared<FJsonObject>());
		if (bIncludeLighting) AddStep(TEXT("editor_build_lighting"), MakeShared<FJsonObject>());
		if (bIncludeNavigation) AddStep(TEXT("editor_build_ai_data"), MakeShared<FJsonObject>());
		if (bIncludePackage) AddStep(TEXT("package_build"), MakeShared<FJsonObject>());

		Out->SetArrayField(TEXT("steps"), Steps);
		Out->SetNumberField(TEXT("step_count"), Steps.Num());
		Out->SetStringField(TEXT("status"), TEXT("planned"));
		Summary = FString::Printf(TEXT("Planned editor build pipeline with %d steps."), Steps.Num());
		return true;
	}
}

void RegisterEditorBuildPipelineTools(FSololmcpToolRegistry& Registry)
{
	using FSB = FSololmcpSchemaBuilder;
	Registry.Register({
		TEXT("editor_build_lighting"),
		TEXT("Trigger editor static-lighting build with selectable quality. Use dry_run=true to preview."),
		FSB::Object({
			{TEXT("quality"), FSB::String(TEXT("Preview, Medium, High, or Production."), {TEXT("Preview"), TEXT("Medium"), TEXT("High"), TEXT("Production")})},
			{TEXT("dry_run"), FSB::Boolean(TEXT("Preview the command without executing it."))}
		}),
		EditorBuildPipeline::ToolBuildLighting
	});
	Registry.Register({
		TEXT("editor_build_navigation"),
		TEXT("Trigger editor navigation/navmesh rebuild for the current level."),
		FSB::Object({{TEXT("dry_run"), FSB::Boolean(TEXT("Preview the command without executing it."))}}),
		EditorBuildPipeline::ToolBuildNavigation
	});
	Registry.Register({
		TEXT("editor_build_ai_data"),
		TEXT("Build AI agent runtime data that is editor-generated today: currently navigation/navmesh plus an explicit AI receipt."),
		FSB::Object({{TEXT("dry_run"), FSB::Boolean(TEXT("Preview the command without executing it."))}}),
		EditorBuildPipeline::ToolBuildAiData
	});
	Registry.Register({
		TEXT("editor_build_reflection_captures"),
		TEXT("Trigger editor reflection-capture build/update for the current level."),
		FSB::Object({{TEXT("dry_run"), FSB::Boolean(TEXT("Preview the command without executing it."))}}),
		EditorBuildPipeline::ToolBuildReflectionCaptures
	});
	Registry.Register({
		TEXT("editor_compile_shaders"),
		TEXT("Trigger shader recompilation from the editor. scope=Changed is safest; All can be expensive."),
		FSB::Object({
			{TEXT("scope"), FSB::String(TEXT("Changed, All, Global, or Material."), {TEXT("Changed"), TEXT("All"), TEXT("Global"), TEXT("Material")})},
			{TEXT("dry_run"), FSB::Boolean(TEXT("Preview the command without executing it."))}
		}),
		EditorBuildPipeline::ToolCompileShaders
	});
	Registry.Register({
		TEXT("packaging_profile_get"),
		TEXT("Read ProjectPackagingSettings from Config/DefaultGame.ini."),
		FSB::Object({}),
		EditorBuildPipeline::ToolPackagingGet,
		nullptr,
		30
	});
	Registry.Register({
		TEXT("packaging_profile_set"),
		TEXT("Write common ProjectPackagingSettings keys: configuration, pak/io store, distribution, debug files, prerequisites, staging dir and maps."),
		FSB::Object({
			{TEXT("build_configuration"), FSB::String(TEXT("Development, Shipping, Test, DebugGame, etc."))},
			{TEXT("staging_directory"), FSB::String(TEXT("Optional package output directory."))},
			{TEXT("use_pak"), FSB::Boolean(TEXT("Set UsePakFile."))},
			{TEXT("use_iostore"), FSB::Boolean(TEXT("Set bUseIoStore."))},
			{TEXT("full_rebuild"), FSB::Boolean(TEXT("Set FullRebuild."))},
			{TEXT("for_distribution"), FSB::Boolean(TEXT("Set ForDistribution."))},
			{TEXT("include_debug_files"), FSB::Boolean(TEXT("Set IncludeDebugFiles."))},
			{TEXT("include_prerequisites"), FSB::Boolean(TEXT("Set IncludePrerequisites."))},
			{TEXT("cook_all"), FSB::Boolean(TEXT("Set bCookAll."))},
			{TEXT("maps_to_cook"), FSB::Array(FSB::String(), TEXT("Long package map paths, e.g. /Game/Maps/Main."))},
			{TEXT("dry_run"), FSB::Boolean(TEXT("Preview config writes without flushing."))}
		}),
		EditorBuildPipeline::ToolPackagingSet
	});
	Registry.Register({
		TEXT("packaging_settings_export_full"),
		TEXT("Export a UE 5.7/5.8-compatible ProjectPackagingSettings snapshot plus known platform settings sections. Supports extra_keys for project-specific fields."),
		FSB::Object({
			{TEXT("platforms"), FSB::Array(FSB::String(), TEXT("Optional target platforms to report sections for: Win64, Linux, Android, IOS, Mac."))},
			{TEXT("extra_keys"), FSB::Array(FSB::String(), TEXT("Additional ProjectPackagingSettings keys to include in the snapshot."))}
		}),
		EditorBuildPipeline::ToolPackagingSettingsExportFull,
		nullptr,
		30
	});
	Registry.Register({
		TEXT("packaging_settings_patch_full"),
		TEXT("Patch arbitrary ProjectPackagingSettings and platform target settings by section/key. This is the full project-settings bridge for UE 5.7/5.8 packaging UI coverage."),
		FSB::Object({
			{TEXT("packaging_settings"), FSB::Object({}, {}, TEXT("Key/value patch for /Script/UnrealEd.ProjectPackagingSettings in DefaultGame.ini."))},
			{TEXT("platform_settings"), FSB::Array(FSB::Object({
				{TEXT("platform"), FSB::String(TEXT("Win64, Linux, Android, IOS, Mac, or custom label."))},
				{TEXT("section"), FSB::String(TEXT("Optional explicit config section. Overrides platform-derived section."))},
				{TEXT("settings"), FSB::Object({}, {}, TEXT("Key/value patch for the platform section."))}
			}), TEXT("Platform-specific DefaultEngine.ini patches."))},
			{TEXT("dry_run"), FSB::Boolean(TEXT("Preview without writing config files."))}
		}),
		EditorBuildPipeline::ToolPackagingSettingsPatchFull
	});
	Registry.Register({
		TEXT("packaging_platform_profile_apply"),
		TEXT("Apply a target-platform packaging profile and return the matching package_build arguments. Covers platform switching, common project packaging flags, MapsToCook, and optional raw platform settings for UE 5.7/5.8."),
		FSB::Object({
			{TEXT("target_platform"), FSB::String(TEXT("Win64, Linux, Android, IOS, Mac, etc."))},
			{TEXT("build_configuration"), FSB::String(TEXT("Development, Shipping, Test, DebugGame."))},
			{TEXT("staging_directory"), FSB::String(TEXT("Optional archive/staging output directory."))},
			{TEXT("use_pak"), FSB::Boolean(TEXT("Set UsePakFile."))},
			{TEXT("use_iostore"), FSB::Boolean(TEXT("Set bUseIoStore."))},
			{TEXT("compressed"), FSB::Boolean(TEXT("Set bCompressed."))},
			{TEXT("for_distribution"), FSB::Boolean(TEXT("Set ForDistribution."))},
			{TEXT("full_rebuild"), FSB::Boolean(TEXT("Set FullRebuild."))},
			{TEXT("skip_editor_content"), FSB::Boolean(TEXT("Set bSkipEditorContent."))},
			{TEXT("maps_to_cook"), FSB::Array(FSB::String(), TEXT("Map long package paths, e.g. /Game/Maps/Main."))},
			{TEXT("additional_packaging_settings"), FSB::Object({}, {}, TEXT("Extra ProjectPackagingSettings key/value patch."))},
			{TEXT("platform_settings"), FSB::Object({}, {}, TEXT("Extra target platform section key/value patch."))},
			{TEXT("dry_run"), FSB::Boolean(TEXT("Preview without writing config files."))}
		}, {TEXT("target_platform")}),
		EditorBuildPipeline::ToolPackagingPlatformProfileApply
	});
	Registry.Register({
		TEXT("packaging_settings_validate"),
		TEXT("Validate packaging settings after platform/profile changes: configuration, Pak/IoStore, Shipping distribution consistency, target platform section, and MapsToCook readiness."),
		FSB::Object({
			{TEXT("target_platform"), FSB::String(TEXT("Optional target platform to validate."))}
		}),
		EditorBuildPipeline::ToolPackagingSettingsValidate,
		nullptr,
		10
	});
	Registry.Register({
		TEXT("project_settings_export"),
		TEXT("Read arbitrary UE project settings from approved project Config files. Covers UE 5.7/5.8 settings without hard-coding every engine property."),
		FSB::Object({
			{TEXT("config_file"), FSB::String(TEXT("DefaultEngine.ini, DefaultGame.ini, DefaultInput.ini, DefaultEditor.ini, or DefaultEditorPerProjectUserSettings.ini."))},
			{TEXT("requests"), FSB::Array(FSB::Object({
				{TEXT("section"), FSB::String(TEXT("Config section, e.g. /Script/Engine.RendererSettings."))},
				{TEXT("keys"), FSB::Array(FSB::String(), TEXT("Keys to read."))}
			}, {TEXT("section"), TEXT("keys")}), TEXT("Section/key read requests."))}
		}, {TEXT("requests")}),
		EditorBuildPipeline::ToolProjectSettingsExport,
		nullptr,
		30
	});
	Registry.Register({
		TEXT("project_settings_patch"),
		TEXT("Patch arbitrary UE project settings in approved project Config files with dry-run support. Values are written as bool, number, string, array, or JSON text."),
		FSB::Object({
			{TEXT("config_file"), FSB::String(TEXT("Approved project Config filename."))},
			{TEXT("patches"), FSB::Array(FSB::Object({
				{TEXT("section"), FSB::String(TEXT("Config section."))},
				{TEXT("settings"), FSB::Object({}, {}, TEXT("Key/value settings patch."))}
			}, {TEXT("section"), TEXT("settings")}), TEXT("Project settings patches."))},
			{TEXT("dry_run"), FSB::Boolean(TEXT("Preview writes without flushing."))}
		}, {TEXT("patches")}),
		EditorBuildPipeline::ToolProjectSettingsPatch
	});
	Registry.Register({
		TEXT("editor_build_pipeline_plan"),
		TEXT("Return a recommended editor build/cook/package sequence for client-side orchestration."),
		FSB::Object({
			{TEXT("include_lighting"), FSB::Boolean()},
			{TEXT("include_navigation"), FSB::Boolean()},
			{TEXT("include_shaders"), FSB::Boolean()},
			{TEXT("include_package"), FSB::Boolean()}
		}),
		EditorBuildPipeline::ToolBuildPipelinePlan,
		nullptr,
		30
	});
}
}
