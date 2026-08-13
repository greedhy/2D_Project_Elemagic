// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 rendering project settings and validation tools.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "AssetToolsModule.h"
#include "AssetCompilingManager.h"
#include "AutomatedAssetImportData.h"
#include "Engine/Engine.h"
#include "Engine/RendererSettings.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "MaterialShared.h"
#include "Modules/ModuleManager.h"
#include "Factories/MaterialFactoryNew.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Components/LightComponent.h"
#include "Engine/ExponentialHeightFog.h"

namespace UE::SOMOLMCP
{
namespace UE58Rendering
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
struct FCVarSpec
{
	const TCHAR* Name;
	const TCHAR* LiteValue;
};

static const FCVarSpec LumenLiteCVars[] = {
	{TEXT("r.Lumen.DiffuseIndirect.Allow"), TEXT("1")},
	{TEXT("r.Lumen.FinalGatherMethod"), TEXT("0")},
	{TEXT("r.Lumen.TraceMeshSDFs.Allow"), TEXT("0")},
	{TEXT("r.LumenScene.SurfaceCache.AtlasSize"), TEXT("2048")},
	{TEXT("r.LumenScene.DirectLighting.MaxLightsPerTile"), TEXT("4")},
	{TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"), TEXT("32")},
	{TEXT("r.Lumen.ScreenProbeGather.RadianceCache.ProbeResolution"), TEXT("16")},
	{TEXT("r.Lumen.HardwareRayTracing.HitLighting.Allowed"), TEXT("0")}
};

static TSharedRef<FJsonObject> ReadCVar(const TCHAR* Name)
{
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("name"), Name);
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		Row->SetBoolField(TEXT("available"), true);
		Row->SetStringField(TEXT("value"), CVar->GetString());
		Row->SetStringField(TEXT("help"), CVar->GetHelp());
	}
	else
	{
		Row->SetBoolField(TEXT("available"), false);
	}
	return Row;
}

static bool SetCVar(const TCHAR* Name, const FString& Value, bool bPersist, FString& Error)
{
	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
	if (!CVar)
	{
		Error = FString::Printf(TEXT("UE 5.8 console variable is unavailable: %s"), Name);
		return false;
	}
	CVar->Set(*Value, ECVF_SetByProjectSetting);
	if (bPersist)
	{
		GConfig->SetString(TEXT("SystemSettings"), Name, *Value, GEngineIni);
		GConfig->Flush(false, GEngineIni);
	}
	return true;
}

static TSharedRef<FJsonObject> SceneRenderingAudit()
{
	TSharedRef<FJsonObject> Audit = MakeShared<FJsonObject>();
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	int32 LightCount = 0;
	int32 MegaLightsEligible = 0;
	int32 DirectionalLights = 0;
	int32 FogCount = 0;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TInlineComponentArray<ULightComponent*> Lights(*It);
			for (ULightComponent* Light : Lights)
			{
				if (!Light) continue;
				++LightCount;
				if (Light->GetClass()->GetName().Contains(TEXT("Directional"))) ++DirectionalLights;
				else ++MegaLightsEligible;
			}
			if (It->IsA<AExponentialHeightFog>()) ++FogCount;
		}
	}
	Audit->SetStringField(TEXT("world"), World ? World->GetPathName() : FString());
	Audit->SetNumberField(TEXT("light_count"), LightCount);
	Audit->SetNumberField(TEXT("megalights_eligible_non_directional_lights"), MegaLightsEligible);
	Audit->SetNumberField(TEXT("directional_light_count"), DirectionalLights);
	Audit->SetNumberField(TEXT("exponential_height_fog_count"), FogCount);
	Audit->SetBoolField(TEXT("hardware_ray_tracing_project_enabled"), GetDefault<URendererSettings>()->bEnableRayTracing != 0);
	return Audit;
}

static void AddLumenProfile(TSharedRef<FJsonObject>& Out)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	int32 Available = 0;
	int32 Matching = 0;
	for (const FCVarSpec& Spec : LumenLiteCVars)
	{
		TSharedRef<FJsonObject> Row = ReadCVar(Spec.Name);
		Row->SetStringField(TEXT("lite_target"), Spec.LiteValue);
		bool bAvailable = false;
		Row->TryGetBoolField(TEXT("available"), bAvailable);
		if (bAvailable)
		{
			++Available;
			FString Value;
			Row->TryGetStringField(TEXT("value"), Value);
			if (Value == Spec.LiteValue) ++Matching;
		}
		Values.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetArrayField(TEXT("cvars"), Values);
	Out->SetNumberField(TEXT("available_count"), Available);
	Out->SetNumberField(TEXT("matching_lite_count"), Matching);
	Out->SetBoolField(TEXT("lite_profile_active"), Available == UE_ARRAY_COUNT(LumenLiteCVars) && Matching == Available);
}

static bool QueueViewportCapture(const FString& Prefix, TSharedRef<FJsonObject>& Out, FString& Error)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("RenderingReceipts"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s.png"), *Prefix, *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%s"))));
	if (!GEngine)
	{
		Error = TEXT("GEngine is unavailable; viewport capture cannot be queued.");
		return false;
	}
	FScreenshotRequest::RequestScreenshot(Path, false, false);
	Out->SetStringField(TEXT("status"), TEXT("queued"));
	Out->SetStringField(TEXT("artifact_path"), Path);
	Out->SetBoolField(TEXT("requires_next_viewport_frame"), true);
	return true;
}

static UMaterial* LoadMaterial(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, FString& AssetPath, FString& Error)
{
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Error = TEXT("asset_path is required.");
		return nullptr;
	}
	UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, Error));
	if (!Material && Error.IsEmpty()) Error = FString::Printf(TEXT("Material was not found: %s"), *AssetPath);
	return Material;
}

static void InspectMaterial(UMaterial* Material, TSharedRef<FJsonObject>& Out)
{
	TArray<TSharedPtr<FJsonValue>> Expressions;
	int32 SubstrateCount = 0;
	int32 ToonCount = 0;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (!Expression) continue;
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Expression->GetName());
		Row->SetStringField(TEXT("class"), Expression->GetClass()->GetPathName());
		const bool bSubstrate = Expression->IsA<UMaterialExpressionSubstrateBSDF>();
		const bool bToon = Expression->IsA<UMaterialExpressionSubstrateToonBSDF>();
		Row->SetBoolField(TEXT("substrate"), bSubstrate);
		Row->SetBoolField(TEXT("toon"), bToon);
		SubstrateCount += bSubstrate ? 1 : 0;
		ToonCount += bToon ? 1 : 0;
		Expressions.Add(MakeShared<FJsonValueObject>(Row));
	}
	TArray<UTexture*> Textures;
	Material->GetUsedTextures(Textures);
	TArray<TSharedPtr<FJsonValue>> TexturePaths;
	for (UTexture* Texture : Textures) if (Texture) TexturePaths.Add(MakeShared<FJsonValueString>(Texture->GetPathName()));
	Out->SetStringField(TEXT("asset_path"), Material->GetPathName());
	Out->SetNumberField(TEXT("expression_count"), Expressions.Num());
	Out->SetNumberField(TEXT("substrate_expression_count"), SubstrateCount);
	Out->SetNumberField(TEXT("toon_expression_count"), ToonCount);
	Out->SetBoolField(TEXT("front_material_connected"), Material->GetEditorOnlyData()->FrontMaterial.Expression != nullptr);
	Out->SetArrayField(TEXT("expressions"), Expressions);
	Out->SetArrayField(TEXT("texture_dependencies"), TexturePaths);
}

static bool CompileMaterial(UMaterial* Material, TSharedRef<FJsonObject>& Out)
{
	Material->PostEditChange();
	Material->ForceRecompileForRendering();
	FAssetCompilingManager::Get().FinishAllCompilation();
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	const bool bValid = !Material->IsCompilingOrHadCompileError(GMaxRHIFeatureLevel);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	Out->SetBoolField(TEXT("compilation_finished"), true);
	Out->SetBoolField(TEXT("compile_succeeded"), bValid);
	return bValid;
}

static bool Execute(const FString& Name, const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	Out->SetStringField(TEXT("tool"), Name);
	Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	if (Name == TEXT("megalights_project_settings_get"))
	{
		const URendererSettings* Settings = GetDefault<URendererSettings>();
		Out->SetBoolField(TEXT("enabled_for_project"), Settings->bEnableMegaLights != 0);
		Out->SetBoolField(TEXT("front_layer_translucency"), Settings->bEnableMegaLightsFrontLayerTranslucency != 0);
		Out->SetObjectField(TEXT("enable_cvar"), ReadCVar(TEXT("r.MegaLights.EnableForProject")));
		Out->SetObjectField(TEXT("translucency_cvar"), ReadCVar(TEXT("r.MegaLights.FrontLayerTranslucency.EnableForProject")));
		Summary = TEXT("MegaLights project settings read from UE 5.8 renderer settings and CVars.");
		return true;
	}
	if (Name == TEXT("megalights_project_settings_set"))
	{
		bool bEnabled = false;
		bool bTranslucency = false;
		Args->TryGetBoolField(TEXT("enabled"), bEnabled);
		Args->TryGetBoolField(TEXT("front_layer_translucency"), bTranslucency);
		URendererSettings* Settings = GetMutableDefault<URendererSettings>();
		Settings->Modify();
		Settings->bEnableMegaLights = bEnabled;
		Settings->bEnableMegaLightsFrontLayerTranslucency = bTranslucency;
		Settings->SaveConfig();
		if (!SetCVar(TEXT("r.MegaLights.EnableForProject"), bEnabled ? TEXT("1") : TEXT("0"), false, Error) ||
			!SetCVar(TEXT("r.MegaLights.FrontLayerTranslucency.EnableForProject"), bTranslucency ? TEXT("1") : TEXT("0"), false, Error)) return false;
		Out->SetBoolField(TEXT("enabled_for_project"), Settings->bEnableMegaLights != 0);
		Out->SetBoolField(TEXT("front_layer_translucency"), Settings->bEnableMegaLightsFrontLayerTranslucency != 0);
		Out->SetBoolField(TEXT("restart_or_shader_recompile_may_be_required"), true);
		Summary = TEXT("MegaLights UE 5.8 project settings were persisted and read back.");
		return true;
	}
	if (Name == TEXT("megalights_scene_compatibility_audit"))
	{
		Out->SetObjectField(TEXT("scene"), SceneRenderingAudit());
		Out->SetBoolField(TEXT("project_enabled"), GetDefault<URendererSettings>()->bEnableMegaLights != 0);
		Out->SetStringField(TEXT("directional_light_policy"), TEXT("MegaLights does not replace directional-light rendering"));
		Summary = TEXT("MegaLights scene compatibility audited against the current editor world.");
		return true;
	}
	if (Name == TEXT("megalights_render_validation_capture"))
	{
		if (!QueueViewportCapture(TEXT("megalights"), Out, Error)) return false;
		Out->SetObjectField(TEXT("scene"), SceneRenderingAudit());
		Summary = TEXT("MegaLights viewport validation capture queued for the next rendered frame.");
		return true;
	}
	if (Name == TEXT("megalights_performance_receipt"))
	{
		Out->SetObjectField(TEXT("scene"), SceneRenderingAudit());
		Out->SetObjectField(TEXT("sample_count_cvar"), ReadCVar(TEXT("r.MegaLights.NumSamplesPerPixel")));
		Out->SetObjectField(TEXT("downsample_mode_cvar"), ReadCVar(TEXT("r.MegaLights.DownsampleMode")));
		Out->SetNumberField(TEXT("app_delta_seconds"), FApp::GetDeltaTime());
		Out->SetNumberField(TEXT("working_set_mb"), FPlatformMemory::GetStats().UsedPhysical / (1024.0 * 1024.0));
		Out->SetBoolField(TEXT("gpu_timing_claimed"), false);
		Summary = TEXT("MegaLights configuration/performance receipt captured without inventing GPU timing data.");
		return true;
	}
	if (Name == TEXT("lumen_lite_capability_probe") || Name == TEXT("lumen_lite_profile_get"))
	{
		AddLumenProfile(Out);
		Out->SetBoolField(TEXT("lumen_project_enabled"), GetDefault<URendererSettings>()->DynamicGlobalIllumination == EDynamicGlobalIlluminationMethod::Lumen);
		Summary = TEXT("Lumen Lite capability/profile read from UE 5.8 renderer CVars.");
		return true;
	}
	if (Name == TEXT("lumen_lite_profile_set"))
	{
		bool bPersist = true;
		Args->TryGetBoolField(TEXT("persist"), bPersist);
		for (const FCVarSpec& Spec : LumenLiteCVars)
		{
			if (!SetCVar(Spec.Name, Spec.LiteValue, bPersist, Error)) return false;
		}
		AddLumenProfile(Out);
		Out->SetBoolField(TEXT("persisted_to_project_config"), bPersist);
		Summary = TEXT("UE 5.8 low-cost Lumen profile applied and read back.");
		return true;
	}
	if (Name == TEXT("lumen_lite_scene_compatibility_audit"))
	{
		AddLumenProfile(Out);
		Out->SetObjectField(TEXT("scene"), SceneRenderingAudit());
		Out->SetBoolField(TEXT("lumen_project_enabled"), GetDefault<URendererSettings>()->DynamicGlobalIllumination == EDynamicGlobalIlluminationMethod::Lumen);
		Summary = TEXT("Current scene audited for the UE 5.8 low-cost Lumen profile.");
		return true;
	}
	if (Name == TEXT("lumen_lite_render_validation_capture"))
	{
		if (!QueueViewportCapture(TEXT("lumen_lite"), Out, Error)) return false;
		AddLumenProfile(Out);
		Summary = TEXT("Lumen Lite viewport validation capture queued for the next rendered frame.");
		return true;
	}
	if (Name == TEXT("lumen_lite_performance_receipt"))
	{
		AddLumenProfile(Out);
		Out->SetNumberField(TEXT("app_delta_seconds"), FApp::GetDeltaTime());
		Out->SetNumberField(TEXT("working_set_mb"), FPlatformMemory::GetStats().UsedPhysical / (1024.0 * 1024.0));
		Out->SetBoolField(TEXT("gpu_timing_claimed"), false);
		Summary = TEXT("Lumen Lite configuration/performance receipt captured without inventing GPU timing data.");
		return true;
	}
	if (Name == TEXT("axf_import_settings_get") || Name == TEXT("axf_import_settings_set"))
	{
		if (!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("InterchangeAxF")))
		{
			Error = TEXT("InterchangeAxF is unavailable. Enable the UE 5.8 Interchange AxF plugin for this project.");
			return false;
		}
		UClass* PipelineClass = FindObject<UClass>(nullptr, TEXT("/Script/InterchangeAxF.AxFInterchangePipeline"));
		if (!PipelineClass)
		{
			Error = TEXT("InterchangeAxF is unavailable. Enable the UE 5.8 Interchange AxF plugin for this project.");
			return false;
		}
		UObject* Pipeline = PipelineClass->GetDefaultObject();
		FBoolProperty* TriplanarProperty = FindFProperty<FBoolProperty>(PipelineClass, TEXT("bUseTriplanarMappingByDefault"));
		if (!TriplanarProperty)
		{
			Error = TEXT("AxF pipeline does not expose bUseTriplanarMappingByDefault.");
			return false;
		}
		if (Name.EndsWith(TEXT("_set")))
		{
			bool bTriplanar = true;
			Args->TryGetBoolField(TEXT("use_triplanar_mapping"), bTriplanar);
			Pipeline->Modify();
			TriplanarProperty->SetPropertyValue_InContainer(Pipeline, bTriplanar);
			Pipeline->SaveConfig();
		}
		Out->SetBoolField(TEXT("interchange_axf_available"), true);
		Out->SetBoolField(TEXT("use_triplanar_mapping"), TriplanarProperty->GetPropertyValue_InContainer(Pipeline));
		Out->SetStringField(TEXT("pipeline_class"), PipelineClass->GetPathName());
		Summary = Name.EndsWith(TEXT("_set")) ? TEXT("AxF Interchange settings persisted and read back.") : TEXT("AxF Interchange settings read back.");
		return true;
	}
	if (Name == TEXT("axf_interchange_import"))
	{
		FString SourceFile;
		FString DestinationPath;
		if (!Args->TryGetStringField(TEXT("source_file"), SourceFile) || !FPaths::FileExists(SourceFile))
		{
			Error = TEXT("source_file must reference an existing .axf file.");
			return false;
		}
		if (!SourceFile.EndsWith(TEXT(".axf"), ESearchCase::IgnoreCase))
		{
			Error = TEXT("AxF import accepts only .axf source files.");
			return false;
		}
		if (!Args->TryGetStringField(TEXT("destination_path"), DestinationPath) || !DestinationPath.StartsWith(TEXT("/Game/")))
		{
			Error = TEXT("destination_path must be a /Game/ content path.");
			return false;
		}
		if (!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("InterchangeAxF")))
		{
			Error = TEXT("InterchangeAxF is unavailable. Enable the UE 5.8 Interchange AxF plugin.");
			return false;
		}
		if (!FindObject<UClass>(nullptr, TEXT("/Script/InterchangeAxF.AxFInterchangePipeline")))
		{
			Error = TEXT("InterchangeAxF is unavailable. Enable the UE 5.8 Interchange AxF plugin.");
			return false;
		}
		UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
		ImportData->GroupName = TEXT("SOMOLMCP_AxF_58");
		ImportData->Filenames = {SourceFile};
		ImportData->DestinationPath = DestinationPath;
		ImportData->bReplaceExisting = false;
		const TArray<UObject*> Imported = FAssetToolsModule::GetModule().Get().ImportAssetsAutomated(ImportData);
		if (Imported.IsEmpty())
		{
			Error = TEXT("AxF Interchange import produced no assets; inspect the UE import log and source-file validity.");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Paths;
		for (UObject* Asset : Imported) if (Asset) Paths.Add(MakeShared<FJsonValueString>(Asset->GetPathName()));
		Out->SetArrayField(TEXT("imported_assets"), Paths);
		Out->SetNumberField(TEXT("imported_count"), Paths.Num());
		Summary = FString::Printf(TEXT("Imported %d AxF assets through UE 5.8 Interchange."), Paths.Num());
		return true;
	}
	if (Name == TEXT("axf_substrate_material_inspect") || Name == TEXT("axf_texture_dependency_readback") ||
		Name == TEXT("axf_substrate_compile_validate") || Name == TEXT("axf_substrate_material_convert"))
	{
		FString AssetPath;
		UMaterial* Material = LoadMaterial(Context, Args, AssetPath, Error);
		if (!Material) return false;
		if (Name == TEXT("axf_substrate_material_convert") && !Material->GetEditorOnlyData()->FrontMaterial.Expression)
		{
			Material->Modify();
			UMaterialExpressionSubstrateSlabBSDF* Slab = NewObject<UMaterialExpressionSubstrateSlabBSDF>(Material);
			Slab->MaterialExpressionEditorX = 0;
			Slab->MaterialExpressionEditorY = 0;
			Material->GetExpressionCollection().AddExpression(Slab);
			Material->GetEditorOnlyData()->FrontMaterial.Connect(0, Slab);
			Material->PostEditChange();
			if (!Context.Services.SaveAsset(AssetPath, false, Error)) return false;
			Out->SetBoolField(TEXT("converted"), true);
		}
		InspectMaterial(Material, Out);
		if (Name == TEXT("axf_substrate_compile_validate"))
		{
			const bool bCompiled = CompileMaterial(Material, Out);
			Out->SetBoolField(TEXT("compiled"), bCompiled);
			if (!bCompiled) { Error = TEXT("AxF/Substrate material compile validation failed."); return false; }
		}
		Summary = FString::Printf(TEXT("AxF/Substrate material processed: %s"), *AssetPath);
		return true;
	}
	if (Name == TEXT("axf_render_qa_capture"))
	{
		if (!QueueViewportCapture(TEXT("axf"), Out, Error)) return false;
		Summary = TEXT("AxF material viewport QA capture queued for the next rendered frame.");
		return true;
	}
	if (Name == TEXT("fog_screen_space_scattering_settings_get") || Name == TEXT("fog_screen_space_scattering_settings_set"))
	{
		if (Name.EndsWith(TEXT("_set")))
		{
			bool bEnabled = true;
			bool bPersist = true;
			double MaxLuminance = 10.0;
			Args->TryGetBoolField(TEXT("enabled"), bEnabled);
			Args->TryGetBoolField(TEXT("persist"), bPersist);
			Args->TryGetNumberField(TEXT("max_exposed_luminance"), MaxLuminance);
			if (!SetCVar(TEXT("r.Fog.ScreenSpaceScattering"), bEnabled ? TEXT("1") : TEXT("0"), bPersist, Error) ||
				!SetCVar(TEXT("r.Fog.ScreenSpaceScattering.MaxExposedLuminance"), FString::SanitizeFloat(FMath::Max(0.1, MaxLuminance)), bPersist, Error)) return false;
			Out->SetBoolField(TEXT("persisted_to_project_config"), bPersist);
		}
		Out->SetObjectField(TEXT("enabled_cvar"), ReadCVar(TEXT("r.Fog.ScreenSpaceScattering")));
		Out->SetObjectField(TEXT("max_exposed_luminance_cvar"), ReadCVar(TEXT("r.Fog.ScreenSpaceScattering.MaxExposedLuminance")));
		Out->SetObjectField(TEXT("taa_cvar"), ReadCVar(TEXT("r.Fog.ScreenSpaceScattering.TAA")));
		Summary = TEXT("Fog Screen Space Scattering settings read back from UE 5.8 CVars.");
		return true;
	}
	if (Name == TEXT("fog_screen_space_scattering_compatibility_audit"))
	{
		Out->SetObjectField(TEXT("scene"), SceneRenderingAudit());
		Out->SetObjectField(TEXT("enabled_cvar"), ReadCVar(TEXT("r.Fog.ScreenSpaceScattering")));
		Out->SetStringField(TEXT("requirement"), TEXT("At least one ExponentialHeightFog and separate fog composition support"));
		Summary = TEXT("Fog Screen Space Scattering compatibility audited against the current scene.");
		return true;
	}
	if (Name == TEXT("fog_screen_space_scattering_render_qa"))
	{
		if (!QueueViewportCapture(TEXT("fog_sss"), Out, Error)) return false;
		Out->SetObjectField(TEXT("scene"), SceneRenderingAudit());
		Summary = TEXT("Fog Screen Space Scattering viewport QA capture queued.");
		return true;
	}
	if (Name == TEXT("substrate_npr_shading_model_catalog"))
	{
		Out->SetStringField(TEXT("expression_class"), UMaterialExpressionSubstrateToonBSDF::StaticClass()->GetPathName());
		Out->SetStringField(TEXT("profile_class"), TEXT("/Script/Engine.ToonProfile"));
		Out->SetBoolField(TEXT("available"), true);
		Summary = TEXT("UE 5.8 Substrate Toon/NPR classes are available.");
		return true;
	}
	if (Name == TEXT("substrate_npr_material_create"))
	{
		FString PackagePath;
		FString AssetName;
		if (!Args->TryGetStringField(TEXT("package_path"), PackagePath) || !PackagePath.StartsWith(TEXT("/Game/")) ||
			!Args->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
		{
			Error = TEXT("package_path (/Game/...) and asset_name are required.");
			return false;
		}
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UMaterial* Material = Cast<UMaterial>(FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory));
		if (!Material) { Error = TEXT("Failed to create Substrate Toon material asset."); return false; }
		Material->Modify();
		UMaterialExpressionSubstrateToonBSDF* Toon = NewObject<UMaterialExpressionSubstrateToonBSDF>(Material);
		Toon->MaterialExpressionEditorX = 0;
		Toon->MaterialExpressionEditorY = 0;
		Material->GetExpressionCollection().AddExpression(Toon);
		Material->GetEditorOnlyData()->FrontMaterial.Connect(0, Toon);
		Material->PostEditChange();
		const FString AssetPath = Material->GetPathName();
		if (!Context.Services.SaveAsset(AssetPath, false, Error)) return false;
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		InspectMaterial(Material, Out);
		Summary = FString::Printf(TEXT("Created UE 5.8 Substrate Toon material: %s"), *AssetPath);
		return true;
	}
	if (Name == TEXT("substrate_npr_material_configure") || Name == TEXT("substrate_npr_compile_validate"))
	{
		FString AssetPath;
		UMaterial* Material = LoadMaterial(Context, Args, AssetPath, Error);
		if (!Material) return false;
		UMaterialExpressionSubstrateToonBSDF* Toon = nullptr;
		for (UMaterialExpression* Expression : Material->GetExpressions()) if ((Toon = Cast<UMaterialExpressionSubstrateToonBSDF>(Expression))) break;
		if (!Toon) { Error = TEXT("Material has no Substrate Toon BSDF expression."); return false; }
		if (Name == TEXT("substrate_npr_material_configure"))
		{
			double Roughness = 0.5;
			Args->TryGetNumberField(TEXT("roughness"), Roughness);
			UMaterialExpressionConstant* Constant = NewObject<UMaterialExpressionConstant>(Material);
			Constant->R = FMath::Clamp(static_cast<float>(Roughness), 0.0f, 1.0f);
			Constant->MaterialExpressionEditorX = -300;
			Constant->MaterialExpressionEditorY = 100;
			Material->GetExpressionCollection().AddExpression(Constant);
			Toon->Roughness.Connect(0, Constant);
			Material->PostEditChange();
			if (!Context.Services.SaveAsset(AssetPath, false, Error)) return false;
		}
		const bool bCompiled = CompileMaterial(Material, Out);
		InspectMaterial(Material, Out);
		Out->SetBoolField(TEXT("compiled"), bCompiled);
		if (!bCompiled) { Error = TEXT("Substrate Toon material compile validation failed."); return false; }
		Summary = FString::Printf(TEXT("Substrate Toon material configured and compiled: %s"), *AssetPath);
		return true;
	}
	if (Name == TEXT("substrate_npr_render_qa_capture"))
	{
		if (!QueueViewportCapture(TEXT("substrate_npr"), Out, Error)) return false;
		Summary = TEXT("Substrate Toon/NPR viewport QA capture queued.");
		return true;
	}
	Error = FString::Printf(TEXT("Unsupported UE 5.8 rendering tool: %s"), *Name);
	return false;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable MegaLights for the project."))},
		{TEXT("front_layer_translucency"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable high-quality MegaLights translucency."))},
		{TEXT("persist"), FSololmcpSchemaBuilder::Boolean(TEXT("Persist the profile to project configuration."))},
		{TEXT("source_file"), FSololmcpSchemaBuilder::String(TEXT("Existing .axf source file."))},
		{TEXT("destination_path"), FSololmcpSchemaBuilder::String(TEXT("Destination /Game/ content path."))},
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path."))},
		{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Destination /Game/ package folder."))},
		{TEXT("asset_name"), FSololmcpSchemaBuilder::String(TEXT("New asset name."))},
		{TEXT("use_triplanar_mapping"), FSololmcpSchemaBuilder::Boolean(TEXT("Use AxF triplanar mapping by default."))},
		{TEXT("max_exposed_luminance"), FSololmcpSchemaBuilder::Number(TEXT("Fog scattering luminance clamp."))},
		{TEXT("roughness"), FSololmcpSchemaBuilder::Number(TEXT("Substrate Toon roughness in [0,1]."))}
	});
}
#endif
}

void RegisterUE58RenderingTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	static const TCHAR* Names[] = {
		TEXT("megalights_project_settings_get"), TEXT("megalights_project_settings_set"),
		TEXT("megalights_scene_compatibility_audit"), TEXT("megalights_render_validation_capture"),
		TEXT("megalights_performance_receipt"), TEXT("lumen_lite_capability_probe"),
		TEXT("lumen_lite_profile_get"), TEXT("lumen_lite_profile_set"),
		TEXT("lumen_lite_scene_compatibility_audit"), TEXT("lumen_lite_render_validation_capture"),
		TEXT("lumen_lite_performance_receipt"), TEXT("axf_interchange_import"),
		TEXT("axf_import_settings_get"), TEXT("axf_import_settings_set"),
		TEXT("axf_substrate_material_convert"), TEXT("axf_substrate_material_inspect"),
		TEXT("axf_texture_dependency_readback"), TEXT("axf_substrate_compile_validate"),
		TEXT("axf_render_qa_capture"), TEXT("fog_screen_space_scattering_settings_get"),
		TEXT("fog_screen_space_scattering_settings_set"), TEXT("fog_screen_space_scattering_compatibility_audit"),
		TEXT("fog_screen_space_scattering_render_qa"), TEXT("substrate_npr_shading_model_catalog"),
		TEXT("substrate_npr_material_create"), TEXT("substrate_npr_material_configure"),
		TEXT("substrate_npr_compile_validate"), TEXT("substrate_npr_render_qa_capture")
	};
	for (const TCHAR* NamePtr : Names)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = FString::Printf(TEXT("UE 5.8 rendering transaction: %s"), *Name);
		Def.InputSchema = UE58Rendering::Schema();
		Def.CacheTtlSeconds = Name.EndsWith(TEXT("_set")) || Name.Contains(TEXT("capture")) ? 0 : 2;
		Def.Execute = [Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return UE58Rendering::Execute(Name, Context, Args, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
#endif
}
}
