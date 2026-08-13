// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpAssetToolkitTools.cpp — SOMOLMCP v1.9.0
// Asset Toolkit: 缩略图传送、资产分析、资产对比、批量查询、引用查找、重命名
//
// UE5.7.4 API adaptations applied:
//   - FRenderTargetInitSettings removed → InitCustomFormat(w, h, format, bLinearGamma)
//   - UThumbnailManager::RenderThumbnail removed → FObjectTools::RenderThumbnail
//   - FObjectThumbnail::GetWidth/GetHeight → GetImageWidth/GetImageHeight
//   - UTexture::GetImportedSize only on UTexture2D, Source returns int64
//   - USkeletalMesh::GetRenderData → GetResourceForRendering
//   - USkeletalMesh::GetAnimBlueprint removed → use GetPostProcessAnimBlueprint
//   - UAnimSequence::GetFrameRate → GetSamplingFrameRate

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpErrorHelpers.h"

// ── UE 核心 ──
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "ActorGroupingUtils.h"
#include "Editor/GroupActor.h"
#include "Engine/Selection.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialFunction.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"

// ── AssetRegistry ──
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/ARFilter.h"

// ── AssetTools ──
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "CollectionManagerModule.h"
#include "ICollectionManager.h"

// ── Thumbnail ──
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ObjectTools.h"  // ThumbnailTools::RenderThumbnail (UE 5.7)
#include "Misc/ObjectThumbnail.h"
#include "Engine/StaticMesh.h"

// ── Image ──
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

// ── Misc ──
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/AssetRegistryInterface.h"
#include "UObject/MetaData.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "Factories/MaterialFactoryNew.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Animation/AnimSequence.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AssetImportTask.h"
#include "ScopedTransaction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorUtilitySubsystem.h"
#include "EditorUtilityBlueprint.h"
#include "EditorUtilityWidget.h"
#include "EditorUtilityWidgetBlueprint.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPAssetToolkit, Log, All);

namespace UE::SOMOLMCP
{

// ICollectionManager gained trailing FText* error out-parameters in 5.5. The pre-5.5
// calls are otherwise identical, so these wrappers keep one call shape and leave the
// error text empty where the engine cannot supply one -- the callers already fall back
// to a generic message when it is empty.
namespace
{
	inline bool SomolIsValidCollectionName(ICollectionManager& M, const FString& Name, ECollectionShareType::Type Share, FText* OutErr)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		return M.IsValidCollectionName(Name, Share, OutErr);
#else
		if (OutErr) { *OutErr = FText::GetEmpty(); }
		return M.IsValidCollectionName(Name, Share);
#endif
	}

	inline bool SomolCreateCollection(ICollectionManager& M, FName Name, ECollectionShareType::Type Share, ECollectionStorageMode::Type Mode, FText* OutErr)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		return M.CreateCollection(Name, Share, Mode, OutErr);
#else
		if (OutErr) { *OutErr = FText::GetEmpty(); }
		return M.CreateCollection(Name, Share, Mode);
#endif
	}

	inline bool SomolSaveCollection(ICollectionManager& M, FName Name, ECollectionShareType::Type Share, FText* OutErr)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		return M.SaveCollection(Name, Share, OutErr);
#else
		if (OutErr) { *OutErr = FText::GetEmpty(); }
		return M.SaveCollection(Name, Share);
#endif
	}

	inline bool SomolAddToCollection(ICollectionManager& M, FName Name, ECollectionShareType::Type Share, TConstArrayView<FSoftObjectPath> Paths, int32* OutNum, FText* OutErr)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		return M.AddToCollection(Name, Share, Paths, OutNum, OutErr);
#else
		if (OutErr) { *OutErr = FText::GetEmpty(); }
		return M.AddToCollection(Name, Share, Paths, OutNum);
#endif
	}

	inline bool SomolRemoveFromCollection(ICollectionManager& M, FName Name, ECollectionShareType::Type Share, TConstArrayView<FSoftObjectPath> Paths, int32* OutNum, FText* OutErr)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		return M.RemoveFromCollection(Name, Share, Paths, OutNum, OutErr);
#else
		if (OutErr) { *OutErr = FText::GetEmpty(); }
		return M.RemoveFromCollection(Name, Share, Paths, OutNum);
#endif
	}
}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 5
	using FSomolObjectMetadata = UMetaData;
	static FSomolObjectMetadata& GetPackageObjectMetadata(UPackage* Package) { return *Package->GetMetaData(); }
	#else
	using FSomolObjectMetadata = FMetaData;
	static FSomolObjectMetadata& GetPackageObjectMetadata(UPackage* Package) { return Package->GetMetaData(); }
	#endif

	// ═══════════════════════════════════════════════════════════════════════
	//  Helpers
	// ═══════════════════════════════════════════════════════════════════════

	/** Convert FAssetData to JSON (replicated from DomainTools since it's in an anonymous namespace there). */
	static TSharedRef<FJsonObject> AssetDataToJsonDetailed(const FAssetData& AssetData)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Json->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
		Json->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
		Json->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());
		Json->SetStringField(TEXT("classPath"), AssetData.AssetClassPath.ToString());

		// Extract tags as metadata
		TSharedRef<FJsonObject> Tags = MakeShared<FJsonObject>();
		for (const auto& TagPair : AssetData.TagsAndValues)
		{
			// UE 5.7: FAssetTagValueRef - use GetValue() to get FString
			Tags->SetStringField(TagPair.Key.ToString(), TagPair.Value.GetValue());
		}
		Json->SetObjectField(TEXT("tags"), Tags);

		return Json;
	}

	/** Load an object from asset path with error handling. */
	static UObject* LoadAssetChecked(const FString& AssetPath, FString& OutError)
	{
		UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath, nullptr, LOAD_None, nullptr);
		if (!Loaded)
		{
			OutError = FString::Printf(TEXT("Failed to load asset: '%s'"), *AssetPath);
		}
		return Loaded;
	}

	static FString NormalizeAssetObjectPath(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.Contains(TEXT(".")) || Path.IsEmpty())
		{
			return Path;
		}

		FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		if (AssetName.IsEmpty())
		{
			AssetName = FPaths::GetBaseFilename(Path);
		}
		return AssetName.IsEmpty() ? Path : FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
	}

	static FString PackageNameFromAssetPath(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		int32 DotIndex = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIndex))
		{
			Path.LeftInline(DotIndex, SOMOLMCP_NO_SHRINK);
		}
		return Path;
	}

	static FAssetData ResolveAssetDataByPath(IAssetRegistry& AssetRegistry, const FString& AssetPath)
	{
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NormalizeAssetObjectPath(AssetPath)));
		if (AssetData.IsValid())
		{
			return AssetData;
		}

		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageNameFromAssetPath(AssetPath)), PackageAssets);
		return PackageAssets.Num() > 0 ? PackageAssets[0] : FAssetData();
	}

	static UObject* LoadAssetFlexible(const FString& AssetPath, FAssetData& OutAssetData, FString& OutResolvedPath, FString& OutError)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		OutAssetData = ResolveAssetDataByPath(AssetRegistry, AssetPath);
		if (OutAssetData.IsValid())
		{
			OutResolvedPath = OutAssetData.GetObjectPathString();
			if (UObject* Loaded = OutAssetData.GetAsset())
			{
				return Loaded;
			}
		}

		OutResolvedPath = NormalizeAssetObjectPath(AssetPath);
		if (UObject* Loaded = LoadObject<UObject>(nullptr, *OutResolvedPath, nullptr, LOAD_None, nullptr))
		{
			return Loaded;
		}

		OutError = FString::Printf(TEXT("Failed to load asset: '%s' (resolved as '%s')"), *AssetPath, *OutResolvedPath);
		return nullptr;
	}

	static TSharedRef<FJsonObject> VectorToJsonLocal(const FVector& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	static TArray<TSharedPtr<FJsonValue>> StringSetToJsonArray(const TSet<FString>& Values)
	{
		TArray<FString> Sorted = Values.Array();
		Sorted.Sort();
		TArray<TSharedPtr<FJsonValue>> Json;
		for (const FString& Value : Sorted)
		{
			Json.Add(MakeShared<FJsonValueString>(Value));
		}
		return Json;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJsonArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		for (const FString& Value : Values)
		{
			Json.Add(MakeShared<FJsonValueString>(Value));
		}
		return Json;
	}

	static int64 GetAssetFileSize(const FString& AssetPath);

	static FString AssetFamilyFromClassName(const FString& ClassName)
	{
		if (ClassName.Contains(TEXT("Texture"))) return TEXT("texture");
		if (ClassName.Contains(TEXT("Material"))) return TEXT("material");
		if (ClassName.Contains(TEXT("StaticMesh"))) return TEXT("static_mesh");
		if (ClassName.Contains(TEXT("SkeletalMesh"))) return TEXT("skeletal_mesh");
		if (ClassName.Contains(TEXT("AnimSequence")) || ClassName.Contains(TEXT("AnimMontage")) || ClassName.Contains(TEXT("BlendSpace"))) return TEXT("animation");
		if (ClassName.Contains(TEXT("Blueprint")) || ClassName.Contains(TEXT("WidgetBlueprint"))) return TEXT("blueprint");
		if (ClassName.Contains(TEXT("Niagara"))) return TEXT("niagara");
		if (ClassName.Contains(TEXT("PCG"))) return TEXT("pcg");
		if (ClassName.Contains(TEXT("Sound")) || ClassName.Contains(TEXT("Audio"))) return TEXT("audio");
		if (ClassName.Contains(TEXT("World")) || ClassName.Contains(TEXT("Level"))) return TEXT("level");
		return TEXT("unknown");
	}

	static FString AssetFamilyFromObject(UObject* Object, const FAssetData& AssetData)
	{
		if (Cast<UTexture>(Object)) return TEXT("texture");
		if (Cast<UMaterialInterface>(Object) || Cast<UMaterialFunction>(Object)) return TEXT("material");
		if (Cast<UStaticMesh>(Object)) return TEXT("static_mesh");
		if (Cast<USkeletalMesh>(Object)) return TEXT("skeletal_mesh");
		if (Cast<UAnimSequence>(Object)) return TEXT("animation");
		if (Cast<UBlueprint>(Object)) return TEXT("blueprint");
		const FString ClassName = Object ? Object->GetClass()->GetName() : AssetData.AssetClassPath.GetAssetName().ToString();
		return AssetFamilyFromClassName(ClassName);
	}

	static FString InferTextureRoleFromName(const FString& NameOrPath)
	{
		const FString Lower = NameOrPath.ToLower();
		if (Lower.Contains(TEXT("normal")) || Lower.EndsWith(TEXT("_n")) || Lower.Contains(TEXT("_n_")) || Lower.Contains(TEXT("_nor"))) return TEXT("normal");
		if (Lower.Contains(TEXT("rough")) || Lower.EndsWith(TEXT("_r")) || Lower.Contains(TEXT("_roughness"))) return TEXT("roughness");
		if (Lower.Contains(TEXT("metal")) || Lower.Contains(TEXT("metallic"))) return TEXT("metallic");
		if (Lower.Contains(TEXT("orm")) || Lower.Contains(TEXT("rma")) || Lower.Contains(TEXT("mrao")) || Lower.Contains(TEXT("mask"))) return TEXT("packed_mask");
		if (Lower.Contains(TEXT("ao")) || Lower.Contains(TEXT("occlusion"))) return TEXT("ambient_occlusion");
		if (Lower.Contains(TEXT("height")) || Lower.Contains(TEXT("disp")) || Lower.Contains(TEXT("displace"))) return TEXT("height");
		if (Lower.Contains(TEXT("opacity")) || Lower.Contains(TEXT("alpha")) || Lower.Contains(TEXT("trans"))) return TEXT("opacity");
		if (Lower.Contains(TEXT("emissive")) || Lower.Contains(TEXT("emit"))) return TEXT("emissive");
		if (Lower.Contains(TEXT("basecolor")) || Lower.Contains(TEXT("albedo")) || Lower.Contains(TEXT("diffuse")) || Lower.EndsWith(TEXT("_bc")) || Lower.EndsWith(TEXT("_d"))) return TEXT("base_color");
		if (Lower.Contains(TEXT("ui")) || Lower.Contains(TEXT("icon")) || Lower.Contains(TEXT("hud"))) return TEXT("ui");
		return TEXT("unknown");
	}

	static void AddSemanticTagsFromPath(const FString& Path, TSet<FString>& Tags)
	{
		const FString Lower = Path.ToLower();
		auto AddIfContains = [&](const TCHAR* Needle, const TCHAR* Tag)
		{
			if (Lower.Contains(Needle))
			{
				Tags.Add(Tag);
			}
		};

		AddIfContains(TEXT("tree"), TEXT("tree"));
		AddIfContains(TEXT("forest"), TEXT("forest"));
		AddIfContains(TEXT("foliage"), TEXT("foliage"));
		AddIfContains(TEXT("grass"), TEXT("grass"));
		AddIfContains(TEXT("rock"), TEXT("rock"));
		AddIfContains(TEXT("stone"), TEXT("stone"));
		AddIfContains(TEXT("cliff"), TEXT("cliff"));
		AddIfContains(TEXT("terrain"), TEXT("terrain"));
		AddIfContains(TEXT("landscape"), TEXT("landscape"));
		AddIfContains(TEXT("character"), TEXT("character"));
		AddIfContains(TEXT("npc"), TEXT("character"));
		AddIfContains(TEXT("weapon"), TEXT("weapon"));
		AddIfContains(TEXT("prop"), TEXT("prop"));
		AddIfContains(TEXT("building"), TEXT("building"));
		AddIfContains(TEXT("ui"), TEXT("ui"));
		AddIfContains(TEXT("vfx"), TEXT("vfx"));
		AddIfContains(TEXT("fx"), TEXT("vfx"));
	}

	static TSharedRef<FJsonObject> BuildAssetRecognitionProfile(
		UObject* Asset,
		const FAssetData& AssetData,
		const FString& RequestedPath,
		const FString& ResolvedPath,
		bool bIncludeDependencies,
		bool bIncludeReferencers,
		int32 MaxDependencies,
		int32 MaxReferencers)
	{
		TSharedRef<FJsonObject> Profile = MakeShared<FJsonObject>();
		const FString ClassName = Asset ? Asset->GetClass()->GetName() : AssetData.AssetClassPath.GetAssetName().ToString();
		const FString Family = AssetFamilyFromObject(Asset, AssetData);

		Profile->SetStringField(TEXT("schema"), TEXT("somol.asset_recognition_profile.v1"));
		Profile->SetStringField(TEXT("requested_path"), RequestedPath);
		Profile->SetStringField(TEXT("resolved_path"), ResolvedPath);
		Profile->SetStringField(TEXT("package_name"), AssetData.IsValid() ? AssetData.PackageName.ToString() : PackageNameFromAssetPath(ResolvedPath));
		Profile->SetStringField(TEXT("asset_name"), Asset ? Asset->GetName() : AssetData.AssetName.ToString());
		Profile->SetStringField(TEXT("class"), ClassName);
		Profile->SetStringField(TEXT("asset_family"), Family);
		Profile->SetNumberField(TEXT("file_size_bytes"), GetAssetFileSize(PackageNameFromAssetPath(ResolvedPath)));

		TSet<FString> SemanticTags;
		SemanticTags.Add(Family);
		AddSemanticTagsFromPath(ResolvedPath, SemanticTags);
		TArray<FString> ReadinessFlags;
		TArray<FString> Warnings;
		TSet<FString> RecommendedTools;

		RecommendedTools.Add(TEXT("asset_get_thumbnail"));
		RecommendedTools.Add(TEXT("asset_analyze"));

		if (UTexture2D* Tex2D = Cast<UTexture2D>(Asset))
		{
			TSharedRef<FJsonObject> TextureProfile = MakeShared<FJsonObject>();
			const FIntPoint ImportedSize = Tex2D->GetImportedSize();
			const int32 Width = ImportedSize.X > 0 ? ImportedSize.X : Tex2D->GetSizeX();
			const int32 Height = ImportedSize.Y > 0 ? ImportedSize.Y : Tex2D->GetSizeY();
			const FString Role = InferTextureRoleFromName(ResolvedPath);
			const bool bPowerOfTwo = FMath::IsPowerOfTwo(Width) && FMath::IsPowerOfTwo(Height);
			TextureProfile->SetNumberField(TEXT("width"), Width);
			TextureProfile->SetNumberField(TEXT("height"), Height);
			TextureProfile->SetBoolField(TEXT("is_square"), Width == Height);
			TextureProfile->SetBoolField(TEXT("is_power_of_two"), bPowerOfTwo);
			TextureProfile->SetStringField(TEXT("inferred_texture_role"), Role);
			TextureProfile->SetStringField(TEXT("compression"), StaticEnum<TextureCompressionSettings>()->GetNameStringByValue(static_cast<int64>(Tex2D->CompressionSettings)));
			TextureProfile->SetBoolField(TEXT("srgb"), Tex2D->SRGB);
			TextureProfile->SetBoolField(TEXT("has_alpha"), Tex2D->HasAlphaChannel());
			TextureProfile->SetNumberField(TEXT("mip_count"), Tex2D->GetNumMips());
			Profile->SetObjectField(TEXT("texture_profile"), TextureProfile);
			SemanticTags.Add(Role);
			RecommendedTools.Add(TEXT("texture_analyze"));
			RecommendedTools.Add(TEXT("material_texture_role_detect"));

			if (!bPowerOfTwo)
			{
				ReadinessFlags.Add(TEXT("texture_non_power_of_two"));
				Warnings.Add(TEXT("Texture dimensions are not power-of-two; UE can use it, but batch/game production may need resizing."));
			}
			if (Role == TEXT("normal") && Tex2D->SRGB)
			{
				ReadinessFlags.Add(TEXT("normal_map_srgb_enabled"));
				Warnings.Add(TEXT("Texture name suggests a normal map, but sRGB is enabled."));
			}
			if ((Role == TEXT("packed_mask") || Role == TEXT("roughness") || Role == TEXT("metallic")) && Tex2D->SRGB)
			{
				ReadinessFlags.Add(TEXT("linear_texture_srgb_enabled"));
				Warnings.Add(TEXT("Texture name suggests a mask/linear data texture, but sRGB is enabled."));
			}
			if (Role == TEXT("unknown"))
			{
				ReadinessFlags.Add(TEXT("unknown_texture_role"));
			}
		}
		else if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
		{
			TSharedRef<FJsonObject> MeshProfile = MakeShared<FJsonObject>();
			const int32 LodCount = SM->GetNumLODs();
			const int32 MatCount = SM->GetStaticMaterials().Num();
			const int32 TriCount = LodCount > 0 ? SM->GetNumTriangles(0) : 0;
			const int32 VertCount = LodCount > 0 ? SM->GetNumVertices(0) : 0;
			const bool bHasCollision = SM->GetBodySetup() != nullptr;
			const FBox Bounds = SM->GetBoundingBox();
			MeshProfile->SetNumberField(TEXT("lod_count"), LodCount);
			MeshProfile->SetNumberField(TEXT("vertex_count_lod0"), VertCount);
			MeshProfile->SetNumberField(TEXT("triangle_count_lod0"), TriCount);
			MeshProfile->SetNumberField(TEXT("material_slot_count"), MatCount);
			MeshProfile->SetBoolField(TEXT("has_collision_body_setup"), bHasCollision);
			MeshProfile->SetBoolField(TEXT("nanite_enabled"), SOMOLMCP_NANITE_SETTINGS(SM).bEnabled);
			MeshProfile->SetNumberField(TEXT("lightmap_resolution"), SM->GetLightMapResolution());
			MeshProfile->SetObjectField(TEXT("bounds_size"), VectorToJsonLocal(Bounds.GetSize()));

			TArray<TSharedPtr<FJsonValue>> Materials;
			for (const FStaticMaterial& StaticMaterial : SM->GetStaticMaterials())
			{
				TSharedRef<FJsonObject> Mat = MakeShared<FJsonObject>();
				Mat->SetStringField(TEXT("slot_name"), StaticMaterial.MaterialSlotName.ToString());
				Mat->SetStringField(TEXT("material"), StaticMaterial.MaterialInterface ? StaticMaterial.MaterialInterface->GetPathName() : TEXT(""));
				Materials.Add(MakeShared<FJsonValueObject>(Mat));
			}
			MeshProfile->SetArrayField(TEXT("materials"), Materials);
			Profile->SetObjectField(TEXT("mesh_profile"), MeshProfile);
			RecommendedTools.Add(TEXT("static_mesh_analyze"));
			RecommendedTools.Add(TEXT("asset_dependency_graph"));
			RecommendedTools.Add(TEXT("pcg_generated_actor_health_audit"));

			if (MatCount == 0)
			{
				ReadinessFlags.Add(TEXT("mesh_no_material_slots"));
				Warnings.Add(TEXT("Static mesh has no material slots."));
			}
			if (!bHasCollision)
			{
				ReadinessFlags.Add(TEXT("mesh_no_collision"));
				Warnings.Add(TEXT("Static mesh has no collision BodySetup; PCG placement is usable but gameplay collision may need repair."));
			}
			if (Bounds.GetSize().IsNearlyZero())
			{
				ReadinessFlags.Add(TEXT("mesh_zero_bounds"));
				Warnings.Add(TEXT("Static mesh bounds are zero or nearly zero."));
			}
			ReadinessFlags.Add(TEXT("pcg_spawn_candidate"));
		}
		else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
		{
			TSharedRef<FJsonObject> SkeletalProfile = MakeShared<FJsonObject>();
			SkeletalProfile->SetNumberField(TEXT("lod_count"), SkeletalMesh->GetLODNum());
			SkeletalProfile->SetNumberField(TEXT("material_slot_count"), SkeletalMesh->GetMaterials().Num());
			SkeletalProfile->SetStringField(TEXT("skeleton"), SkeletalMesh->GetSkeleton() ? SkeletalMesh->GetSkeleton()->GetPathName() : TEXT(""));
			SkeletalProfile->SetStringField(TEXT("physics_asset"), SkeletalMesh->GetPhysicsAsset() ? SkeletalMesh->GetPhysicsAsset()->GetPathName() : TEXT(""));
			Profile->SetObjectField(TEXT("skeletal_mesh_profile"), SkeletalProfile);
			RecommendedTools.Add(TEXT("skeletal_mesh_analyze"));
			RecommendedTools.Add(TEXT("animation_asset_compat_diagnose"));
			if (!SkeletalMesh->GetSkeleton())
			{
				ReadinessFlags.Add(TEXT("skeletal_mesh_missing_skeleton"));
				Warnings.Add(TEXT("Skeletal mesh has no skeleton; animation binding cannot be trusted."));
			}
		}
		else if (UAnimSequence* AnimSeq = Cast<UAnimSequence>(Asset))
		{
			TSharedRef<FJsonObject> AnimProfile = MakeShared<FJsonObject>();
			AnimProfile->SetNumberField(TEXT("duration"), AnimSeq->GetPlayLength());
			AnimProfile->SetNumberField(TEXT("frame_count"), AnimSeq->GetNumberOfSampledKeys());
			AnimProfile->SetNumberField(TEXT("frame_rate"), AnimSeq->GetSamplingFrameRate().AsDecimal());
			AnimProfile->SetStringField(TEXT("skeleton"), AnimSeq->GetSkeleton() ? AnimSeq->GetSkeleton()->GetPathName() : TEXT(""));
			Profile->SetObjectField(TEXT("animation_profile"), AnimProfile);
			RecommendedTools.Add(TEXT("animation_asset_compat_diagnose"));
			if (!AnimSeq->GetSkeleton())
			{
				ReadinessFlags.Add(TEXT("animation_missing_skeleton"));
			}
		}
		else if (UMaterialInterface* MatInterface = Cast<UMaterialInterface>(Asset))
		{
			TSharedRef<FJsonObject> MaterialProfile = MakeShared<FJsonObject>();
			MaterialProfile->SetStringField(TEXT("material_class"), MatInterface->GetClass()->GetName());
			if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Asset))
			{
				MaterialProfile->SetStringField(TEXT("parent"), MIC->Parent ? MIC->Parent->GetPathName() : TEXT(""));
				MaterialProfile->SetNumberField(TEXT("scalar_parameter_count"), MIC->ScalarParameterValues.Num());
				MaterialProfile->SetNumberField(TEXT("vector_parameter_count"), MIC->VectorParameterValues.Num());
				MaterialProfile->SetNumberField(TEXT("texture_parameter_count"), MIC->TextureParameterValues.Num());
			}
			if (UMaterial* Mat = Cast<UMaterial>(Asset))
			{
				MaterialProfile->SetBoolField(TEXT("two_sided"), Mat->IsTwoSided());
				MaterialProfile->SetBoolField(TEXT("shading_model_from_material_expression"), Mat->IsShadingModelFromMaterialExpression());
			}
			Profile->SetObjectField(TEXT("material_profile"), MaterialProfile);
			RecommendedTools.Add(TEXT("material_inspect"));
			RecommendedTools.Add(TEXT("material_graph_explain"));
			RecommendedTools.Add(TEXT("material_get_statistics"));
		}
		else if (UBlueprint* BP = Cast<UBlueprint>(Asset))
		{
			TSharedRef<FJsonObject> BlueprintProfile = MakeShared<FJsonObject>();
			BlueprintProfile->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT(""));
			BlueprintProfile->SetStringField(TEXT("generated_class"), BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : TEXT(""));
			BlueprintProfile->SetNumberField(TEXT("variable_count"), BP->NewVariables.Num());
			Profile->SetObjectField(TEXT("blueprint_profile"), BlueprintProfile);
			RecommendedTools.Add(TEXT("blueprint_inspect_summary"));
			RecommendedTools.Add(TEXT("blueprint_compile_diagnostics"));
		}

		if (bIncludeDependencies || bIncludeReferencers)
		{
			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

			if (bIncludeDependencies)
			{
				TArray<FName> Dependencies;
				AssetRegistry.GetDependencies(FName(*PackageNameFromAssetPath(ResolvedPath)), Dependencies);
				TArray<TSharedPtr<FJsonValue>> DependencyRows;
				TMap<FString, int32> CountsByFamily;
				int32 TextureDeps = 0;
				for (int32 i = 0; i < Dependencies.Num(); ++i)
				{
					TArray<FAssetData> DepAssets;
					AssetRegistry.GetAssetsByPackageName(Dependencies[i], DepAssets);
					const FAssetData DepData = DepAssets.Num() > 0 ? DepAssets[0] : FAssetData();
					const FString DepClass = DepData.IsValid() ? DepData.AssetClassPath.GetAssetName().ToString() : TEXT("Unknown");
					const FString DepFamily = AssetFamilyFromClassName(DepClass);
					CountsByFamily.FindOrAdd(DepFamily)++;
					if (DepFamily == TEXT("texture")) TextureDeps++;

					if (DependencyRows.Num() < MaxDependencies)
					{
						TSharedRef<FJsonObject> Dep = MakeShared<FJsonObject>();
						Dep->SetStringField(TEXT("package"), Dependencies[i].ToString());
						Dep->SetStringField(TEXT("object_path"), DepData.IsValid() ? DepData.GetObjectPathString() : TEXT(""));
						Dep->SetStringField(TEXT("class"), DepClass);
						Dep->SetStringField(TEXT("family"), DepFamily);
						DependencyRows.Add(MakeShared<FJsonValueObject>(Dep));
					}
				}

				TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
				for (const auto& Pair : CountsByFamily)
				{
					Counts->SetNumberField(Pair.Key, Pair.Value);
				}
				TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
				Summary->SetNumberField(TEXT("total"), Dependencies.Num());
				Summary->SetNumberField(TEXT("returned"), DependencyRows.Num());
				Summary->SetObjectField(TEXT("counts_by_family"), Counts);
				Summary->SetArrayField(TEXT("dependencies"), DependencyRows);
				Profile->SetObjectField(TEXT("dependency_summary"), Summary);

				if (Family == TEXT("material") && TextureDeps == 0)
				{
					ReadinessFlags.Add(TEXT("material_no_texture_dependencies_detected"));
				}
			}

			if (bIncludeReferencers)
			{
				TArray<FName> Referencers;
				AssetRegistry.GetReferencers(FName(*PackageNameFromAssetPath(ResolvedPath)), Referencers);
				TArray<TSharedPtr<FJsonValue>> ReferencerRows;
				for (int32 i = 0; i < FMath::Min(Referencers.Num(), MaxReferencers); ++i)
				{
					ReferencerRows.Add(MakeShared<FJsonValueString>(Referencers[i].ToString()));
				}
				Profile->SetArrayField(TEXT("referencers"), ReferencerRows);
				Profile->SetNumberField(TEXT("total_referencers"), Referencers.Num());
			}
		}

		if (Family == TEXT("pcg"))
		{
			RecommendedTools.Add(TEXT("pcg_graph_validate"));
			RecommendedTools.Add(TEXT("pcg_graph_explain"));
			RecommendedTools.Add(TEXT("pcg_dry_run"));
		}
		if (Family == TEXT("niagara"))
		{
			RecommendedTools.Add(TEXT("niagara_compile_diagnostics"));
			RecommendedTools.Add(TEXT("niagara_runtime_snapshot"));
		}

		TSharedRef<FJsonObject> PcgReadiness = MakeShared<FJsonObject>();
		const bool bPcgSpawnReady = Cast<UStaticMesh>(Asset) != nullptr && !ReadinessFlags.Contains(TEXT("mesh_zero_bounds"));
		PcgReadiness->SetBoolField(TEXT("usable_as_pcg_spawn_mesh"), bPcgSpawnReady);
		PcgReadiness->SetBoolField(TEXT("needs_asset_profile_before_generation"), false);
		PcgReadiness->SetArrayField(TEXT("blocking_flags"), StringArrayToJsonArray(ReadinessFlags));
		Profile->SetObjectField(TEXT("pcg_readiness"), PcgReadiness);

		Profile->SetArrayField(TEXT("semantic_tags"), StringSetToJsonArray(SemanticTags));
		Profile->SetArrayField(TEXT("readiness_flags"), StringArrayToJsonArray(ReadinessFlags));
		Profile->SetArrayField(TEXT("warnings"), StringArrayToJsonArray(Warnings));
		Profile->SetArrayField(TEXT("recommended_next_tools"), StringSetToJsonArray(RecommendedTools));
		Profile->SetObjectField(TEXT("registry_data"), AssetData.IsValid() ? AssetDataToJsonDetailed(AssetData) : MakeShared<FJsonObject>());
		return Profile;
	}

	/** Get the on-disk file size of an asset package. */
	static int64 GetAssetFileSize(const FString& AssetPath)
	{
		FString PackageFileName;
		// UE 5.7: DoesPackageExist deprecated Guid parameter, use simpler overload
		if (!FPackageName::DoesPackageExist(*AssetPath, &PackageFileName))
		{
			return 0;
		}
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		return PlatformFile.FileSize(*PackageFileName);
	}

	/** Extract common properties from a UObject into JSON. */
	static TSharedRef<FJsonObject> ExtractCommonProperties(UObject* Object)
	{
		TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

		if (!Object) return Props;

		// Class info
		if (UClass* Class = Object->GetClass())
		{
			Props->SetStringField(TEXT("className"), Class->GetName());
			Props->SetStringField(TEXT("classPath"), Class->GetPathName());

			// Blueprint parent class
			if (UBlueprint* BP = Cast<UBlueprint>(Object))
			{
				if (BP->ParentClass)
				{
					Props->SetStringField(TEXT("parentClass"), BP->ParentClass->GetName());
				}
				Props->SetStringField(TEXT("blueprintType"), UEnum::GetDisplayValueAsText(BP->BlueprintType).ToString());
			}
		}

		// StaticMesh properties
		if (UStaticMesh* SM = Cast<UStaticMesh>(Object))
		{
			Props->SetStringField(TEXT("assetType"), TEXT("StaticMesh"));
			if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD0 = SM->GetRenderData()->LODResources[0];
				Props->SetNumberField(TEXT("vertexCount"), LOD0.GetNumVertices());
				Props->SetNumberField(TEXT("triangleCount"), LOD0.GetNumTriangles());
				Props->SetNumberField(TEXT("lodCount"), SM->GetRenderData()->LODResources.Num());

				// Vertex buffer size estimate (approximate: position + normal + tangent + UV + color)
				const int32 BytesPerVertex = 48; // conservative estimate
				const int32 VertexBufferSize = LOD0.GetNumVertices() * BytesPerVertex;
				const int32 IndexBufferSize = LOD0.GetNumTriangles() * 3 * sizeof(uint32);
				Props->SetNumberField(TEXT("gpuMemoryEstimateKB"), (VertexBufferSize + IndexBufferSize) / 1024);
			}

			// Bounding box
			Props->SetStringField(TEXT("boundingBox"),
				FString::Printf(TEXT("[%.2f, %.2f, %.2f] x [%.2f, %.2f, %.2f]"),
					SM->GetBoundingBox().Min.X, SM->GetBoundingBox().Min.Y, SM->GetBoundingBox().Min.Z,
					SM->GetBoundingBox().Max.X, SM->GetBoundingBox().Max.Y, SM->GetBoundingBox().Max.Z));
		}

		// SkeletalMesh properties
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object))
		{
			Props->SetStringField(TEXT("assetType"), TEXT("SkeletalMesh"));
			// UE 5.7: FSkeletalMeshRenderData removed; use LODInfo for LOD count
			Props->SetNumberField(TEXT("lodCount"), SkeletalMesh->GetLODNum());
			if (SkeletalMesh->GetSkeleton())
			{
				Props->SetStringField(TEXT("skeleton"), SkeletalMesh->GetSkeleton()->GetPathName());
			}
			// UE5.7.4: GetAnimBlueprint removed — use GetPostProcessAnimBlueprint if needed
			if (SkeletalMesh->GetPostProcessAnimBlueprint())
			{
				Props->SetStringField(TEXT("postProcessAnimBlueprint"), SkeletalMesh->GetPostProcessAnimBlueprint()->GetPathName());
			}
		}

		// Material properties
		if (UMaterialInterface* MatInterface = Cast<UMaterialInterface>(Object))
		{
			Props->SetStringField(TEXT("assetType"), TEXT("Material"));
			if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Object))
			{
				Props->SetStringField(TEXT("materialType"), TEXT("MaterialInstance"));
				if (MIC->Parent)
				{
					Props->SetStringField(TEXT("parentMaterial"), MIC->Parent->GetPathName());
				}
				Props->SetNumberField(TEXT("parameterCount"), MIC->ScalarParameterValues.Num() + MIC->VectorParameterValues.Num() + MIC->TextureParameterValues.Num());
			}
			else if (UMaterialFunction* MatFunc = Cast<UMaterialFunction>(Object))
			{
				Props->SetStringField(TEXT("materialType"), TEXT("MaterialFunction"));
			}
			else if (UMaterial* Mat = Cast<UMaterial>(Object))
			{
				Props->SetStringField(TEXT("materialType"), TEXT("Material"));
				Props->SetBoolField(TEXT("isShadingModelFromMaterial"), Mat->IsShadingModelFromMaterialExpression());
				Props->SetBoolField(TEXT("isTwoSided"), Mat->IsTwoSided());
			}
		}

		// Texture properties
		if (UTexture* Tex = Cast<UTexture>(Object))
		{
			Props->SetStringField(TEXT("assetType"), TEXT("Texture"));
			// UE5.7.4: Source is in WITH_EDITORONLY_DATA; check IsValid before use
#if WITH_EDITORONLY_DATA
			if (Tex->Source.IsValid())
			{
				Props->SetNumberField(TEXT("sourceWidth"), (double)Tex->Source.GetSizeX());
				Props->SetNumberField(TEXT("sourceHeight"), (double)Tex->Source.GetSizeY());
			}
#endif
			// UE5.7.4: GetImportedSize only exists on UTexture2D
			if (UTexture2D* Tex2D = Cast<UTexture2D>(Tex))
			{
				FIntPoint ImportedSize = Tex2D->GetImportedSize();
				Props->SetNumberField(TEXT("width"), ImportedSize.X);
				Props->SetNumberField(TEXT("height"), ImportedSize.Y);

				Props->SetStringField(TEXT("textureFormat"), UEnum::GetDisplayValueAsText(Tex2D->GetPixelFormat()).ToString());
				Props->SetNumberField(TEXT("mipCount"), Tex2D->GetNumMips());
				Props->SetBoolField(TEXT("sRGB"), Tex2D->SRGB);
				Props->SetBoolField(TEXT("compressionNone"), Tex2D->CompressionNone);
				Props->SetBoolField(TEXT("hasAlphaChannel"), Tex2D->HasAlphaChannel());
			}
		}

		// AnimSequence properties
		if (UAnimSequence* AnimSeq = Cast<UAnimSequence>(Object))
		{
			Props->SetStringField(TEXT("assetType"), TEXT("AnimSequence"));
			Props->SetNumberField(TEXT("frameCount"), AnimSeq->GetNumberOfSampledKeys());
			Props->SetNumberField(TEXT("duration"), AnimSeq->GetPlayLength());
			// UE5.7.4: GetFrameRate removed, use GetSamplingFrameRate
			Props->SetNumberField(TEXT("frameRate"), AnimSeq->GetSamplingFrameRate().AsDecimal());
		}

		// PhysicsAsset properties
		if (UPhysicsAsset* PhysAsset = Cast<UPhysicsAsset>(Object))
		{
			Props->SetStringField(TEXT("assetType"), TEXT("PhysicsAsset"));
			Props->SetNumberField(TEXT("skeletalBodySetupCount"), PhysAsset->SkeletalBodySetups.Num());
			Props->SetNumberField(TEXT("constraintSetupCount"), PhysAsset->ConstraintSetup.Num());
		}

		return Props;
	}

	/** Get all assets that reference a given asset. */
	static void GetReferencers(const FString& AssetPath, TArray<FName>& OutReferencers)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.GetReferencers(*AssetPath, OutReferencers);
	}

	/** Get all assets that a given asset references (dependencies). */
	static void GetDependencies(const FString& AssetPath, TArray<FName>& OutDependencies)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.GetDependencies(*AssetPath, OutDependencies);
	}

	static bool TryGetAssetPathArgument(const TSharedRef<FJsonObject>& Arguments, FString& OutAssetPath, FString& OutError)
	{
		if (!Arguments->TryGetStringField(TEXT("asset_path"), OutAssetPath))
		{
			Arguments->TryGetStringField(TEXT("target_asset"), OutAssetPath);
		}
		OutAssetPath.TrimStartAndEndInline();
		if (OutAssetPath.IsEmpty())
		{
			OutError = TEXT("Missing argument: asset_path (or target_asset)");
			return false;
		}
		return true;
	}

	static bool ResolveAssetDataForPromotedTool(const TSharedRef<FJsonObject>& Arguments, FAssetData& OutAssetData, FString& OutRequestedPath, FString& OutError)
	{
		if (!TryGetAssetPathArgument(Arguments, OutRequestedPath, OutError))
		{
			return false;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		OutAssetData = ResolveAssetDataByPath(AssetRegistry, OutRequestedPath);
		if (!OutAssetData.IsValid())
		{
			OutError = FString::Printf(TEXT("Asset not found in registry: '%s'"), *OutRequestedPath);
			return false;
		}
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> NameListToJsonArray(const TArray<FName>& Names, int32 MaxResults)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 Limit = FMath::Clamp(MaxResults, 1, 1000);
		for (int32 Index = 0; Index < FMath::Min(Names.Num(), Limit); ++Index)
		{
			Values.Add(MakeShared<FJsonValueString>(Names[Index].ToString()));
		}
		return Values;
	}

	static TArray<TSharedPtr<FJsonValue>> PackageNameRowsToJson(IAssetRegistry& AssetRegistry, const TArray<FName>& PackageNames, int32 MaxResults)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		const int32 Limit = FMath::Clamp(MaxResults, 1, 1000);
		for (int32 Index = 0; Index < FMath::Min(PackageNames.Num(), Limit); ++Index)
		{
			const FName PackageName = PackageNames[Index];
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("packageName"), PackageName.ToString());

			TArray<FAssetData> AssetsInPackage;
			AssetRegistry.GetAssetsByPackageName(PackageName, AssetsInPackage);
			if (AssetsInPackage.Num() > 0)
			{
				Row->SetBoolField(TEXT("assetResolved"), true);
				Row->SetObjectField(TEXT("asset"), AssetDataToJsonDetailed(AssetsInPackage[0]));
				if (AssetsInPackage.Num() > 1)
				{
					Row->SetNumberField(TEXT("additionalAssetsInPackage"), AssetsInPackage.Num() - 1);
				}
			}
			else
			{
				Row->SetBoolField(TEXT("assetResolved"), false);
			}

			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	static TSharedRef<FJsonObject> EditorSubsystemCatalogRow(const FString& Name, const FString& ClassPath, const FString& Capability, const FString& Notes)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Name);
		Row->SetStringField(TEXT("classPath"), ClassPath);
		Row->SetStringField(TEXT("capability"), Capability);
		Row->SetStringField(TEXT("notes"), Notes);
		Row->SetBoolField(TEXT("available"), FindObject<UClass>(nullptr, *ClassPath) != nullptr);
		return Row;
	}

	static bool GetPromotedExecuteFlag(const TSharedRef<FJsonObject>& Arguments)
	{
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		return bExecute;
	}

	static bool BuildPromotedDestinationPackagePath(
		const TSharedRef<FJsonObject>& Arguments,
		const FAssetData& SourceAssetData,
		FString& OutDestinationPackagePath,
		FString& OutError,
		const bool bDefaultNameToSourceAsset = false)
	{
		FString DestinationPath;
		if (!Arguments->TryGetStringField(TEXT("destination_asset_path"), DestinationPath) &&
			!Arguments->TryGetStringField(TEXT("destination_path"), DestinationPath) &&
			!Arguments->TryGetStringField(TEXT("dest_path"), DestinationPath))
		{
			FString NewName;
			if (!Arguments->TryGetStringField(TEXT("new_name"), NewName))
			{
				if (bDefaultNameToSourceAsset)
				{
					NewName = SourceAssetData.AssetName.ToString();
				}
				else
				{
					OutError = TEXT("Missing destination: provide destination_asset_path/destination_path or new_name.");
					return false;
				}
			}
			NewName.TrimStartAndEndInline();
			if (NewName.IsEmpty())
			{
				OutError = TEXT("new_name cannot be empty.");
				return false;
			}

			FString DestinationFolder;
			if (!Arguments->TryGetStringField(TEXT("destination_folder"), DestinationFolder))
			{
				DestinationFolder = FPackageName::GetLongPackagePath(SourceAssetData.PackageName.ToString());
			}
			DestinationFolder.TrimStartAndEndInline();
			if (DestinationFolder.IsEmpty())
			{
				OutError = TEXT("destination_folder cannot be empty.");
				return false;
			}
			DestinationPath = DestinationFolder / NewName;
		}

		DestinationPath.TrimStartAndEndInline();
		DestinationPath = PackageNameFromAssetPath(DestinationPath);
		if (DestinationPath.IsEmpty() || !DestinationPath.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(TEXT("Destination must be a long package path under a mounted root, got '%s'."), *DestinationPath);
			return false;
		}
		if (!FPackageName::IsValidLongPackageName(DestinationPath))
		{
			OutError = FString::Printf(TEXT("Invalid destination long package name: '%s'."), *DestinationPath);
			return false;
		}
		if (FPackageName::GetLongPackageAssetName(DestinationPath).IsEmpty())
		{
			OutError = FString::Printf(TEXT("Destination has no asset name: '%s'."), *DestinationPath);
			return false;
		}

		OutDestinationPackagePath = DestinationPath;
		return true;
	}

	static bool DestinationAssetExists(IAssetRegistry& AssetRegistry, const FString& DestinationPackagePath)
	{
		return ResolveAssetDataByPath(AssetRegistry, DestinationPackagePath).IsValid();
	}

	static ECollectionShareType::Type ParseCollectionShareType(const TSharedRef<FJsonObject>& Arguments)
	{
		FString ShareTypeText = TEXT("Local");
		Arguments->TryGetStringField(TEXT("share_type"), ShareTypeText);
		ShareTypeText.TrimStartAndEndInline();
		if (ShareTypeText.Equals(TEXT("private"), ESearchCase::IgnoreCase))
		{
			return ECollectionShareType::CST_Private;
		}
		if (ShareTypeText.Equals(TEXT("shared"), ESearchCase::IgnoreCase))
		{
			return ECollectionShareType::CST_Shared;
		}
		return ECollectionShareType::CST_Local;
	}

	static ECollectionStorageMode::Type ParseCollectionStorageMode(const TSharedRef<FJsonObject>& Arguments)
	{
		FString StorageModeText = TEXT("Static");
		Arguments->TryGetStringField(TEXT("storage_mode"), StorageModeText);
		StorageModeText.TrimStartAndEndInline();
		return StorageModeText.Equals(TEXT("dynamic"), ESearchCase::IgnoreCase)
			? ECollectionStorageMode::Dynamic
			: ECollectionStorageMode::Static;
	}

	static bool GetCollectionNameArgument(const TSharedRef<FJsonObject>& Arguments, FName& OutCollectionName, FString& OutError)
	{
		FString CollectionNameText;
		if (!Arguments->TryGetStringField(TEXT("collection_name"), CollectionNameText) &&
			!Arguments->TryGetStringField(TEXT("name"), CollectionNameText))
		{
			OutError = TEXT("Missing argument: collection_name");
			return false;
		}
		CollectionNameText.TrimStartAndEndInline();
		if (CollectionNameText.IsEmpty())
		{
			OutError = TEXT("collection_name cannot be empty.");
			return false;
		}
		OutCollectionName = FName(*CollectionNameText);
		return true;
	}

	static bool TryGetStringArrayArgument(const TSharedRef<FJsonObject>& Arguments, const FString& FieldName, TArray<FString>& OutValues);

	static FString ActorLabelForJson(AActor* Actor)
	{
		return Actor ? Actor->GetActorLabel() : FString();
	}

	static TSharedRef<FJsonObject> ActorToJsonLocal(AActor* Actor)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Actor)
		{
			Json->SetBoolField(TEXT("valid"), false);
			return Json;
		}
		Json->SetBoolField(TEXT("valid"), true);
		Json->SetStringField(TEXT("name"), Actor->GetName());
		Json->SetStringField(TEXT("label"), ActorLabelForJson(Actor));
		Json->SetStringField(TEXT("path"), Actor->GetPathName());
		Json->SetStringField(TEXT("classPath"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
		Json->SetObjectField(TEXT("location"), VectorToJsonLocal(Actor->GetActorLocation()));
		if (AGroupActor* ParentGroup = Cast<AGroupActor>(Actor->GroupActor.Get()))
		{
			Json->SetStringField(TEXT("parentGroup"), ParentGroup->GetPathName());
			Json->SetStringField(TEXT("parentGroupLabel"), ActorLabelForJson(ParentGroup));
		}
		return Json;
	}

	static TSharedRef<FJsonObject> GroupActorToJsonLocal(AGroupActor* GroupActor, bool bIncludeMembers = true)
	{
		TSharedRef<FJsonObject> Json = ActorToJsonLocal(GroupActor);
		if (!GroupActor)
		{
			return Json;
		}
		Json->SetBoolField(TEXT("locked"), GroupActor->IsLocked());
		if (bIncludeMembers)
		{
			TArray<AActor*> DirectActors;
			GroupActor->GetGroupActors(DirectActors, false);
			TArray<AActor*> RecursiveActors;
			GroupActor->GetGroupActors(RecursiveActors, true);
			TArray<TSharedPtr<FJsonValue>> DirectRows;
			for (AActor* Actor : DirectActors)
			{
				DirectRows.Add(MakeShared<FJsonValueObject>(ActorToJsonLocal(Actor)));
			}
			TArray<TSharedPtr<FJsonValue>> RecursiveRows;
			for (AActor* Actor : RecursiveActors)
			{
				RecursiveRows.Add(MakeShared<FJsonValueObject>(ActorToJsonLocal(Actor)));
			}
			Json->SetArrayField(TEXT("directActors"), DirectRows);
			Json->SetArrayField(TEXT("allActors"), RecursiveRows);
			Json->SetNumberField(TEXT("directActorCount"), DirectActors.Num());
			Json->SetNumberField(TEXT("allActorCount"), RecursiveActors.Num());
		}
		return Json;
	}

	static void GetActorIdentifierArguments(const TSharedRef<FJsonObject>& Arguments, TArray<FString>& OutIdentifiers)
	{
		if (!TryGetStringArrayArgument(Arguments, TEXT("actors"), OutIdentifiers) &&
			!TryGetStringArrayArgument(Arguments, TEXT("actor_ids"), OutIdentifiers))
		{
			TryGetStringArrayArgument(Arguments, TEXT("actor_names"), OutIdentifiers);
		}
		FString SingleActor;
		if (Arguments->TryGetStringField(TEXT("actor"), SingleActor) ||
			Arguments->TryGetStringField(TEXT("actor_id"), SingleActor) ||
			Arguments->TryGetStringField(TEXT("actor_name"), SingleActor))
		{
			SingleActor.TrimStartAndEndInline();
			if (!SingleActor.IsEmpty())
			{
				OutIdentifiers.Add(SingleActor);
			}
		}
	}

	static AActor* ResolveActorByIdentifier(UWorld* World, const FString& Identifier, FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("World is unavailable.");
			return nullptr;
		}
		const FString Query = Identifier.TrimStartAndEnd();
		if (Query.IsEmpty())
		{
			OutError = TEXT("Actor identifier is empty.");
			return nullptr;
		}

		TArray<AActor*> PartialMatches;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			const FString ActorName = Actor->GetName();
			const FString ActorLabel = ActorLabelForJson(Actor);
			const FString ActorPath = Actor->GetPathName();
			if (ActorName.Equals(Query, ESearchCase::IgnoreCase) ||
				ActorLabel.Equals(Query, ESearchCase::IgnoreCase) ||
				ActorPath.Equals(Query, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
			if (ActorName.Contains(Query, ESearchCase::IgnoreCase) ||
				ActorLabel.Contains(Query, ESearchCase::IgnoreCase) ||
				ActorPath.Contains(Query, ESearchCase::IgnoreCase))
			{
				PartialMatches.Add(Actor);
			}
		}
		if (PartialMatches.Num() == 1)
		{
			return PartialMatches[0];
		}
		if (PartialMatches.Num() > 1)
		{
			OutError = FString::Printf(TEXT("Actor identifier '%s' is ambiguous (%d partial matches). Use an exact actor name, label, or path."), *Query, PartialMatches.Num());
			return nullptr;
		}
		OutError = FString::Printf(TEXT("Actor not found: '%s'."), *Query);
		return nullptr;
	}

	static bool ResolveActorListForGroupTool(
		const TSharedRef<FJsonObject>& Arguments,
		UWorld* World,
		bool bAllowSelection,
		TArray<AActor*>& OutActors,
		TArray<TSharedPtr<FJsonValue>>& OutRows,
		TArray<TSharedPtr<FJsonValue>>& OutProblems,
		FString& OutError)
	{
		TArray<FString> Identifiers;
		GetActorIdentifierArguments(Arguments, Identifiers);
		bool bUseSelection = false;
		Arguments->TryGetBoolField(TEXT("use_selection"), bUseSelection);
		if (Identifiers.Num() == 0 && bAllowSelection && bUseSelection && GEditor)
		{
			if (USelection* Selection = GEditor->GetSelectedActors())
			{
				for (FSelectionIterator It(*Selection); It; ++It)
				{
					if (AActor* Actor = Cast<AActor>(*It))
					{
						if (!OutActors.Contains(Actor))
						{
							OutActors.Add(Actor);
							OutRows.Add(MakeShared<FJsonValueObject>(ActorToJsonLocal(Actor)));
						}
					}
				}
			}
			return true;
		}
		if (Identifiers.Num() == 0)
		{
			OutError = bAllowSelection ? TEXT("Missing actors. Provide actors/actor_ids/actor_names or set use_selection=true.") : TEXT("Missing actors. Provide actors/actor_ids/actor_names.");
			return false;
		}

		for (const FString& Identifier : Identifiers)
		{
			FString ActorError;
			AActor* Actor = ResolveActorByIdentifier(World, Identifier, ActorError);
			if (!Actor)
			{
				TSharedRef<FJsonObject> Problem = MakeShared<FJsonObject>();
				Problem->SetStringField(TEXT("identifier"), Identifier);
				Problem->SetStringField(TEXT("error"), ActorError);
				OutProblems.Add(MakeShared<FJsonValueObject>(Problem));
				continue;
			}
			if (!OutActors.Contains(Actor))
			{
				OutActors.Add(Actor);
				OutRows.Add(MakeShared<FJsonValueObject>(ActorToJsonLocal(Actor)));
			}
		}
		return true;
	}

	static bool ResolveGroupActorArgument(const TSharedRef<FJsonObject>& Arguments, UWorld* World, AGroupActor*& OutGroupActor, FString& OutError)
	{
		FString GroupIdentifier;
		if (!Arguments->TryGetStringField(TEXT("group_actor"), GroupIdentifier) &&
			!Arguments->TryGetStringField(TEXT("group_actor_id"), GroupIdentifier) &&
			!Arguments->TryGetStringField(TEXT("group_name"), GroupIdentifier))
		{
			OutError = TEXT("Missing group_actor/group_actor_id/group_name.");
			return false;
		}
		GroupIdentifier.TrimStartAndEndInline();
		AActor* Actor = ResolveActorByIdentifier(World, GroupIdentifier, OutError);
		OutGroupActor = Cast<AGroupActor>(Actor);
		if (!OutGroupActor)
		{
			OutError = FString::Printf(TEXT("Actor '%s' is not an AGroupActor."), *GroupIdentifier);
			return false;
		}
		return true;
	}

	static bool GetCollectionAssetPathsArgument(
		const TSharedRef<FJsonObject>& Arguments,
		TArray<FSoftObjectPath>& OutObjectPaths,
		TArray<TSharedPtr<FJsonValue>>& OutResolvedRows,
		TArray<TSharedPtr<FJsonValue>>& OutMissingRows,
		FString& OutError)
	{
		TArray<FString> RawPaths;
		const TArray<TSharedPtr<FJsonValue>>* AssetPathValues = nullptr;
		if (Arguments->TryGetArrayField(TEXT("asset_paths"), AssetPathValues))
		{
			for (const TSharedPtr<FJsonValue>& Value : *AssetPathValues)
			{
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					RawPaths.Add(Value->AsString());
				}
			}
		}
		else
		{
			FString SinglePath;
			if (TryGetAssetPathArgument(Arguments, SinglePath, OutError))
			{
				RawPaths.Add(SinglePath);
			}
			else
			{
				OutError = TEXT("Missing argument: asset_paths or asset_path");
				return false;
			}
		}

		if (RawPaths.Num() == 0)
		{
			OutError = TEXT("No asset paths supplied.");
			return false;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		for (FString Path : RawPaths)
		{
			Path.TrimStartAndEndInline();
			FAssetData AssetData = ResolveAssetDataByPath(AssetRegistry, Path);
			if (AssetData.IsValid())
			{
				OutObjectPaths.Add(FSoftObjectPath(AssetData.GetObjectPathString()));
				OutResolvedRows.Add(MakeShared<FJsonValueObject>(AssetDataToJsonDetailed(AssetData)));
			}
			else
			{
				TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
				Missing->SetStringField(TEXT("requestedPath"), Path);
				Missing->SetStringField(TEXT("reason"), TEXT("asset_not_found"));
				OutMissingRows.Add(MakeShared<FJsonValueObject>(Missing));
			}
		}

		return true;
	}

	static bool TryGetStringArrayArgument(const TSharedRef<FJsonObject>& Arguments, const FString& FieldName, TArray<FString>& OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Arguments->TryGetArrayField(FieldName, Values))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				FString Text = Value->AsString();
				Text.TrimStartAndEndInline();
				if (!Text.IsEmpty())
				{
					OutValues.Add(Text);
				}
			}
		}
		return true;
	}

	static bool JsonValueToMetadataString(const TSharedPtr<FJsonValue>& Value, FString& OutValue, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("metadata value is null or invalid.");
			return false;
		}
		if (Value->Type == EJson::String)
		{
			OutValue = Value->AsString();
			return true;
		}
		if (Value->Type == EJson::Number)
		{
			OutValue = FString::SanitizeFloat(Value->AsNumber());
			return true;
		}
		if (Value->Type == EJson::Boolean)
		{
			OutValue = Value->AsBool() ? TEXT("true") : TEXT("false");
			return true;
		}
		if (Value->Type == EJson::Null)
		{
			OutValue = FString();
			return true;
		}
		OutError = TEXT("metadata values must be string, number, boolean, or null; nested objects/arrays are not accepted by this safe writer.");
		return false;
	}

	static bool ParseMetadataPatchArguments(
		const TSharedRef<FJsonObject>& Arguments,
		TMap<FName, FString>& OutSetValues,
		TArray<FName>& OutRemoveKeys,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* MetadataObjectPtr = nullptr;
		if (Arguments->TryGetObjectField(TEXT("metadata"), MetadataObjectPtr) && MetadataObjectPtr && MetadataObjectPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& MetadataObject = *MetadataObjectPtr;
			for (const auto& Pair : MetadataObject->Values)
			{
				FString KeyText(*Pair.Key);
				KeyText.TrimStartAndEndInline();
				if (KeyText.IsEmpty())
				{
					OutError = TEXT("metadata contains an empty key.");
					return false;
				}
				FString MetadataValue;
				if (!JsonValueToMetadataString(Pair.Value, MetadataValue, OutError))
				{
					OutError = FString::Printf(TEXT("Invalid metadata value for key '%s': %s"), *KeyText, *OutError);
					return false;
				}
				OutSetValues.Add(FName(*KeyText), MetadataValue);
			}
		}

		FString KeyText;
		if (Arguments->TryGetStringField(TEXT("key"), KeyText))
		{
			KeyText.TrimStartAndEndInline();
			if (KeyText.IsEmpty())
			{
				OutError = TEXT("key cannot be empty.");
				return false;
			}
			const TSharedPtr<FJsonValue>* ValuePtr = Arguments->Values.Find(TEXT("value"));
			if (!ValuePtr)
			{
				OutError = TEXT("key was provided but value is missing.");
				return false;
			}
			FString MetadataValue;
			if (!JsonValueToMetadataString(*ValuePtr, MetadataValue, OutError))
			{
				OutError = FString::Printf(TEXT("Invalid metadata value for key '%s': %s"), *KeyText, *OutError);
				return false;
			}
			OutSetValues.Add(FName(*KeyText), MetadataValue);
		}

		TArray<FString> RemoveKeyStrings;
		TryGetStringArrayArgument(Arguments, TEXT("remove_keys"), RemoveKeyStrings);
		for (FString RemoveKey : RemoveKeyStrings)
		{
			RemoveKey.TrimStartAndEndInline();
			if (!RemoveKey.IsEmpty())
			{
				OutRemoveKeys.Add(FName(*RemoveKey));
			}
		}

		if (OutSetValues.Num() == 0 && OutRemoveKeys.Num() == 0)
		{
			OutError = TEXT("No metadata changes supplied. Provide metadata, key/value, or remove_keys.");
			return false;
		}
		return true;
	}

	static TSharedRef<FJsonObject> ObjectMetadataToJson(UObject* Asset)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Asset)
		{
			return Json;
		}

		if (UPackage* Package = Asset->GetOutermost())
		{
			Package->GetMetaData();
		}
		if (TMap<FName, FString>* MetadataMap = FSomolObjectMetadata::GetMapForObject(Asset))
		{
			TArray<FName> Keys;
			MetadataMap->GetKeys(Keys);
			Keys.Sort(FNameLexicalLess());
			for (const FName& Key : Keys)
			{
				Json->SetStringField(Key.ToString(), MetadataMap->FindChecked(Key));
			}
		}
		return Json;
	}

	static TArray<TSharedPtr<FJsonValue>> MetadataPatchRows(FSomolObjectMetadata& Metadata, UObject* Asset, const TMap<FName, FString>& SetValues, const TArray<FName>& RemoveKeys)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<FName> SetKeys;
		SetValues.GetKeys(SetKeys);
		SetKeys.Sort(FNameLexicalLess());
		for (const FName& Key : SetKeys)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			const FString* Existing = Metadata.FindValue(Asset, Key);
			Row->SetStringField(TEXT("key"), Key.ToString());
			Row->SetStringField(TEXT("operation"), TEXT("set"));
			Row->SetBoolField(TEXT("hadPreviousValue"), Existing != nullptr);
			if (Existing)
			{
				Row->SetStringField(TEXT("previousValue"), *Existing);
			}
			Row->SetStringField(TEXT("newValue"), SetValues.FindChecked(Key));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		for (const FName& Key : RemoveKeys)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			const FString* Existing = Metadata.FindValue(Asset, Key);
			Row->SetStringField(TEXT("key"), Key.ToString());
			Row->SetStringField(TEXT("operation"), TEXT("remove"));
			Row->SetBoolField(TEXT("hadPreviousValue"), Existing != nullptr);
			if (Existing)
			{
				Row->SetStringField(TEXT("previousValue"), *Existing);
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	static void ApplyMetadataPatch(FSomolObjectMetadata& Metadata, UObject* Asset, const TMap<FName, FString>& SetValues, const TArray<FName>& RemoveKeys)
	{
		TMap<FName, FString> FinalValues;
		if (TMap<FName, FString>* Existing = FSomolObjectMetadata::GetMapForObject(Asset))
		{
			FinalValues = *Existing;
		}
		for (const auto& Pair : SetValues)
		{
			FinalValues.Add(Pair.Key, Pair.Value);
		}
		for (const FName& Key : RemoveKeys)
		{
			FinalValues.Remove(Key);
		}
		Metadata.SetObjectValues(Asset, MoveTemp(FinalValues));
	}

	static FString ImportKindFromExtension(const FString& SourceFile)
	{
		const FString Ext = FPaths::GetExtension(SourceFile).ToLower();
		if (Ext == TEXT("fbx") || Ext == TEXT("obj") || Ext == TEXT("gltf") || Ext == TEXT("glb")) return TEXT("mesh_or_scene");
		if (Ext == TEXT("png") || Ext == TEXT("jpg") || Ext == TEXT("jpeg") || Ext == TEXT("tga") || Ext == TEXT("exr") || Ext == TEXT("hdr")) return TEXT("texture");
		if (Ext == TEXT("wav") || Ext == TEXT("aif") || Ext == TEXT("aiff") || Ext == TEXT("flac") || Ext == TEXT("ogg")) return TEXT("audio");
		if (Ext == TEXT("uasset")) return TEXT("unreal_asset_package");
		if (Ext == TEXT("abc")) return TEXT("alembic");
		if (Ext == TEXT("usd") || Ext == TEXT("usda") || Ext == TEXT("usdc")) return TEXT("usd");
		return Ext.IsEmpty() ? TEXT("unknown") : Ext;
	}

	static bool BuildImportTaskPlanRows(
		const TArray<FString>& SourceFiles,
		const FString& DestinationPath,
		const TArray<FString>& DestinationNames,
		TArray<TSharedPtr<FJsonValue>>& OutRows,
		TArray<TSharedPtr<FJsonValue>>& OutProblems,
		TArray<FString>& OutPlannedPackagePaths)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		for (int32 Index = 0; Index < SourceFiles.Num(); ++Index)
		{
			const FString& SourceFile = SourceFiles[Index];
			const FString DestinationName = DestinationNames.IsValidIndex(Index) && !DestinationNames[Index].IsEmpty()
				? DestinationNames[Index]
				: FPaths::GetBaseFilename(SourceFile);
			const FString PlannedPackagePath = DestinationPath / DestinationName;
			OutPlannedPackagePaths.Add(PlannedPackagePath);

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("sourceFile"), SourceFile);
			Row->SetStringField(TEXT("destinationPath"), DestinationPath);
			Row->SetStringField(TEXT("destinationName"), DestinationName);
			Row->SetStringField(TEXT("plannedPackagePath"), PlannedPackagePath);
			Row->SetStringField(TEXT("extension"), FPaths::GetExtension(SourceFile).ToLower());
			Row->SetStringField(TEXT("importKind"), ImportKindFromExtension(SourceFile));
			Row->SetBoolField(TEXT("sourceExists"), FPaths::FileExists(SourceFile));
			Row->SetBoolField(TEXT("targetExists"), DestinationAssetExists(AssetRegistry, PlannedPackagePath));
			OutRows.Add(MakeShared<FJsonValueObject>(Row));

			if (!FPaths::FileExists(SourceFile))
			{
				TSharedRef<FJsonObject> Problem = MakeShared<FJsonObject>();
				Problem->SetStringField(TEXT("sourceFile"), SourceFile);
				Problem->SetStringField(TEXT("reason"), TEXT("source_file_missing"));
				OutProblems.Add(MakeShared<FJsonValueObject>(Problem));
			}
		}
		return OutProblems.Num() == 0;
	}

	/** Render a thumbnail for the given UObject and return PNG data. */
	static bool RenderThumbnail(UObject* Asset, int32 MaxWidth, int32 MaxHeight, TArray<uint8>& OutPngData, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("Asset is null.");
			return false;
		}

		if (!GEditor)
		{
			OutError = TEXT("GEditor not available (thumbnails require editor context).");
			return false;
		}

		// UE5.7: ThumbnailTools::RenderThumbnail (was FObjectTools::RenderThumbnail in older UE)
		const int32 RenderWidth = FMath::Max(MaxWidth, 64);
		const int32 RenderHeight = FMath::Max(MaxHeight, 64);

		FObjectThumbnail TempThumbnail;
		// UE 5.7: EThumbnailTextureFlushMode enum removed, use default flush mode
		ThumbnailTools::RenderThumbnail(Asset, RenderWidth, RenderHeight, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &TempThumbnail);

		// UE5.7.4: Use GetImageWidth/GetImageHeight (not GetWidth/GetHeight)
		if (TempThumbnail.GetImageWidth() > 0 && TempThumbnail.GetImageHeight() > 0 && TempThumbnail.AccessImageData().Num() > 0)
		{
			// Decompress thumbnail data (may be JPEG/PNG compressed)
			const TArray<uint8>& UncompressedData = TempThumbnail.GetUncompressedImageData();
			if (UncompressedData.Num() == 0)
			{
				OutError = FString::Printf(TEXT("Failed to decompress thumbnail for asset '%s'."), *Asset->GetName());
				return false;
			}

			const int32 ThumbWidth = TempThumbnail.GetImageWidth();
			const int32 ThumbHeight = TempThumbnail.GetImageHeight();

			// The thumbnail data is BGRA8 format
			TArray<FColor> Colors;
			Colors.Reserve(ThumbWidth * ThumbHeight);
			for (int32 i = 0; i < ThumbWidth * ThumbHeight; ++i)
			{
				const int32 ByteIdx = i * 4;
				if (ByteIdx + 3 < UncompressedData.Num())
				{
					// BGRA -> RGBA
					Colors.Add(FColor(
						UncompressedData[ByteIdx + 2],  // R
						UncompressedData[ByteIdx + 1],  // G
						UncompressedData[ByteIdx],      // B
						UncompressedData[ByteIdx + 3]   // A
					));
				}
				else
				{
					Colors.Add(FColor::Black);
				}
			}

			// Compress to PNG
			bool bRendered = FSololmcpEditorServices::CompressPixelsToPng(Colors, ThumbWidth, ThumbHeight, OutPngData, OutError);
			if (!bRendered)
			{
				return false;
			}

			return true;
		}

		OutError = FString::Printf(TEXT("Failed to render thumbnail for asset '%s'. The asset type may not support thumbnail rendering, or no preview is available."), *Asset->GetName());
		return false;
	}

	// ═══════════════════════════════════════════════════════════════════════
	//  RegisterAssetToolkitTools — asset search, analysis, recognition, thumbnails, references, rename
	// ═══════════════════════════════════════════════════════════════════════

	void RegisterAssetToolkitTools(FSololmcpToolRegistry& Registry)
	{
		// ── 1. asset_get_thumbnail ──
		// 返回资产的缩略图（base64 PNG），AI 可以"看到"资产外观

		Registry.Register({
			TEXT("asset_get_thumbnail"),
			TEXT("Get the thumbnail/preview image of an Unreal Engine asset as a base64-encoded PNG. This allows the AI to visually analyze the asset's appearance. Works with StaticMesh, SkeletalMesh, Material, Texture, Blueprint, and other asset types."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Full object path of the asset, e.g. '/Game/Props/SM_Chair'"))},
					{TEXT("max_width"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum thumbnail width in pixels. Default 512."))},
					{TEXT("max_height"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum thumbnail height in pixels. Default 512."))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				int32 MaxWidth = 512;
				int32 MaxHeight = 512;
				Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
				Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);
				MaxWidth = FMath::Clamp(MaxWidth, 64, 2048);
				MaxHeight = FMath::Clamp(MaxHeight, 64, 2048);

				UObject* Asset = LoadAssetChecked(AssetPath, OutError);
				if (!Asset) return false;

				TArray<uint8> PngData;
				if (!RenderThumbnail(Asset, MaxWidth, MaxHeight, PngData, OutError))
				{
					return false;
				}

				// Return as MCP image content (same pattern as screenshot tools)
				TArray<TSharedPtr<FJsonValue>> ImageContent;
				ImageContent.Add(MakeImageContentValue(PngData));
				OutStructured->SetArrayField(TEXT("_imageContent"), ImageContent);
				OutStructured->SetStringField(TEXT("assetPath"), AssetPath);
				OutStructured->SetStringField(TEXT("assetName"), Asset->GetName());
				OutStructured->SetNumberField(TEXT("image_size_bytes"), PngData.Num());
				OutSummary = FString::Printf(TEXT("Thumbnail of asset '%s' (%d bytes)."), *AssetPath, PngData.Num());
				return true;
			}
		});

		// ── 2. asset_analyze ──
		// 深入分析资产属性、文件大小、引用关系等

		Registry.Register({
			TEXT("asset_analyze"),
			TEXT("Analyze an Unreal Engine asset in depth. Returns detailed metadata including class info, file size on disk, property breakdown (vertices/triangles for meshes, resolution for textures, etc.), references, and dependents. This provides comprehensive information about an asset for decision-making."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Full object path of the asset, e.g. '/Game/Props/SM_Chair'"))},
					{TEXT("include_references"), FSololmcpSchemaBuilder::Boolean(TEXT("Include list of assets this asset references (dependencies). Default true."))},
					{TEXT("include_referencers"), FSololmcpSchemaBuilder::Boolean(TEXT("Include list of assets that reference this asset (dependents). Default false."))},
					{TEXT("max_references"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum number of references/referencers to return. Default 50."))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				const bool bIncludeRefs = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_references"))
					? Arguments->GetBoolField(TEXT("include_references")) : true;
				const bool bIncludeReferrers = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_referencers"))
					? Arguments->GetBoolField(TEXT("include_referencers")) : false;
				int32 MaxRefs = 50;
				Arguments->TryGetNumberField(TEXT("max_references"), MaxRefs);
				MaxRefs = FMath::Clamp(MaxRefs, 1, 500);

				UObject* Asset = LoadAssetChecked(AssetPath, OutError);
				if (!Asset) return false;

				// Basic asset info
				OutStructured->SetStringField(TEXT("assetPath"), AssetPath);
				OutStructured->SetStringField(TEXT("assetName"), Asset->GetName());
				OutStructured->SetNumberField(TEXT("fileSizeBytes"), GetAssetFileSize(AssetPath));

				// Extract type-specific properties
				TSharedRef<FJsonObject> Properties = ExtractCommonProperties(Asset);
				OutStructured->SetObjectField(TEXT("properties"), Properties);

				// Get AssetRegistry data for tags
				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				FAssetData AssetData;
				// UE 5.7: Use FSoftObjectPath version of GetAssetByObjectPath
				FSoftObjectPath SoftPath(AssetPath);
				AssetData = AssetRegistry.GetAssetByObjectPath(SoftPath);
				if (AssetData.IsValid())
				{
					OutStructured->SetObjectField(TEXT("registryData"), AssetDataToJsonDetailed(AssetData));
				}

				// Dependencies (what this asset references)
				if (bIncludeRefs)
				{
					TArray<FName> Dependencies;
					GetDependencies(AssetPath, Dependencies);
					TArray<TSharedPtr<FJsonValue>> DepsJson;
					for (int32 i = 0; i < FMath::Min(Dependencies.Num(), MaxRefs); ++i)
					{
						DepsJson.Add(MakeShared<FJsonValueString>(Dependencies[i].ToString()));
					}
					OutStructured->SetArrayField(TEXT("dependencies"), DepsJson);
					OutStructured->SetNumberField(TEXT("totalDependencies"), Dependencies.Num());
					if (Dependencies.Num() > MaxRefs)
					{
						OutStructured->SetNumberField(TEXT("truncatedDependencies"), Dependencies.Num() - MaxRefs);
					}
				}

				// Referencers (what references this asset)
				if (bIncludeReferrers)
				{
					TArray<FName> Referencers;
					GetReferencers(AssetPath, Referencers);
					TArray<TSharedPtr<FJsonValue>> RefsJson;
					for (int32 i = 0; i < FMath::Min(Referencers.Num(), MaxRefs); ++i)
					{
						RefsJson.Add(MakeShared<FJsonValueString>(Referencers[i].ToString()));
					}
					OutStructured->SetArrayField(TEXT("referencers"), RefsJson);
					OutStructured->SetNumberField(TEXT("totalReferencers"), Referencers.Num());
					if (Referencers.Num() > MaxRefs)
					{
						OutStructured->SetNumberField(TEXT("truncatedReferencers"), Referencers.Num() - MaxRefs);
					}
				}

				OutSummary = FString::Printf(TEXT("Analyzed asset '%s' — %lld bytes on disk."), *AssetPath, GetAssetFileSize(AssetPath));
				return true;
			}
		});

		// ── 3. asset_recognition_profile ──
		// Agent-facing asset perception profile for PCG/material/animation/scene pipelines.
		Registry.Register({
			TEXT("asset_recognition_profile"),
			TEXT("Build an Agent-facing recognition profile for a single UE asset. Resolves package or object paths, classifies the asset family, infers semantic tags and texture roles, summarizes dependencies, flags production/PCG readiness risks, and returns recommended next MCP tools. Use this before PCG scattering, material wiring, animation binding, generated-asset deployment, or large-scene content planning."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Package or object path, e.g. '/Game/Props/SM_Tree' or '/Game/Props/SM_Tree.SM_Tree'."))},
					{TEXT("include_dependencies"), FSololmcpSchemaBuilder::Boolean(TEXT("Include direct dependency summary. Default true."))},
					{TEXT("include_referencers"), FSololmcpSchemaBuilder::Boolean(TEXT("Include referencer package list. Default false."))},
					{TEXT("max_dependencies"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum dependency rows to return. Default 80, max 500."))},
					{TEXT("max_referencers"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum referencer rows to return. Default 40, max 500."))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				bool bIncludeDependencies = true;
				bool bIncludeReferencers = false;
				Arguments->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
				Arguments->TryGetBoolField(TEXT("include_referencers"), bIncludeReferencers);

				int32 MaxDependencies = 80;
				int32 MaxReferencers = 40;
				Arguments->TryGetNumberField(TEXT("max_dependencies"), MaxDependencies);
				Arguments->TryGetNumberField(TEXT("max_referencers"), MaxReferencers);
				MaxDependencies = FMath::Clamp(MaxDependencies, 0, 500);
				MaxReferencers = FMath::Clamp(MaxReferencers, 0, 500);

				FAssetData AssetData;
				FString ResolvedPath;
				UObject* Asset = LoadAssetFlexible(AssetPath, AssetData, ResolvedPath, OutError);
				if (!Asset)
				{
					return false;
				}

				OutStructured = BuildAssetRecognitionProfile(
					Asset,
					AssetData,
					AssetPath,
					ResolvedPath,
					bIncludeDependencies,
					bIncludeReferencers,
					MaxDependencies,
					MaxReferencers);

				OutSummary = FString::Printf(TEXT("Recognized asset '%s' as %s (%s)."),
					*ResolvedPath,
					*OutStructured->GetStringField(TEXT("asset_family")),
					*OutStructured->GetStringField(TEXT("class")));
				return true;
			}
		});

		// ── 4. asset_compare ──
		// 对比两个资产的属性差异，并返回双缩略图

		Registry.Register({
			TEXT("asset_compare"),
			TEXT("Compare two Unreal Engine assets side by side. Returns property differences (class, size, vertices, resolution, etc.) and thumbnail images for visual comparison. Ideal for comparing LOD variants, material versions, mesh iterations, or similar assets."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path_a"), FSololmcpSchemaBuilder::String(TEXT("Full object path of the first asset."))},
					{TEXT("asset_path_b"), FSololmcpSchemaBuilder::String(TEXT("Full object path of the second asset."))},
					{TEXT("include_thumbnails"), FSololmcpSchemaBuilder::Boolean(TEXT("Include thumbnail images for visual comparison. Default true."))},
					{TEXT("thumbnail_size"), FSololmcpSchemaBuilder::Integer(TEXT("Thumbnail size in pixels (square). Default 256."))}
				},
				{TEXT("asset_path_a"), TEXT("asset_path_b")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPathA, AssetPathB;
				if (!Arguments->TryGetStringField(TEXT("asset_path_a"), AssetPathA) ||
					!Arguments->TryGetStringField(TEXT("asset_path_b"), AssetPathB))
				{
					OutError = TEXT("Missing arguments: asset_path_a and asset_path_b");
					return false;
				}

				const bool bIncludeThumbnails = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_thumbnails"))
					? Arguments->GetBoolField(TEXT("include_thumbnails")) : true;
				int32 ThumbSize = 256;
				Arguments->TryGetNumberField(TEXT("thumbnail_size"), ThumbSize);
				ThumbSize = FMath::Clamp(ThumbSize, 64, 1024);

				UObject* AssetA = LoadAssetChecked(AssetPathA, OutError);
				if (!AssetA) return false;
				UObject* AssetB = LoadAssetChecked(AssetPathB, OutError);
				if (!AssetB) return false;

				// Extract properties for both
				TSharedRef<FJsonObject> PropsA = ExtractCommonProperties(AssetA);
				TSharedRef<FJsonObject> PropsB = ExtractCommonProperties(AssetB);
				PropsA->SetStringField(TEXT("objectPath"), AssetPathA);
				PropsB->SetStringField(TEXT("objectPath"), AssetPathB);

				OutStructured->SetObjectField(TEXT("assetA"), PropsA);
				OutStructured->SetObjectField(TEXT("assetB"), PropsB);

				// File size comparison
				int64 SizeA = GetAssetFileSize(AssetPathA);
				int64 SizeB = GetAssetFileSize(AssetPathB);
				OutStructured->SetNumberField(TEXT("assetA.fileSizeBytes"), SizeA);
				OutStructured->SetNumberField(TEXT("assetB.fileSizeBytes"), SizeB);
				OutStructured->SetNumberField(TEXT("sizeDifferenceBytes"), SizeA - SizeB);

				// Compute property differences
				TArray<TSharedPtr<FJsonValue>> Differences;
				auto CompareField = [&](const FString& Key)
				{
					FString ValA, ValB;
					PropsA->TryGetStringField(Key, ValA);
					PropsB->TryGetStringField(Key, ValB);
					if (ValA != ValB)
					{
						TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>();
						Diff->SetStringField(TEXT("field"), Key);
						Diff->SetStringField(TEXT("assetA"), ValA);
						Diff->SetStringField(TEXT("assetB"), ValB);
						Differences.Add(MakeShared<FJsonValueObject>(Diff));
					}
				};
				auto CompareNumField = [&](const FString& Key)
				{
					double ValA = 0, ValB = 0;
					bool HasA = PropsA->TryGetNumberField(Key, ValA);
					bool HasB = PropsB->TryGetNumberField(Key, ValB);
					if (HasA != HasB || (HasA && FMath::Abs(ValA - ValB) > 0.001))
					{
						TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>();
						Diff->SetStringField(TEXT("field"), Key);
						if (HasA) Diff->SetStringField(TEXT("assetA"), FString::Printf(TEXT("%.2f"), ValA));
						if (HasB) Diff->SetStringField(TEXT("assetB"), FString::Printf(TEXT("%.2f"), ValB));
						Differences.Add(MakeShared<FJsonValueObject>(Diff));
					}
				};

				// Compare common fields
				CompareField(TEXT("className"));
				CompareField(TEXT("assetType"));
				CompareField(TEXT("parentClass"));
				CompareNumField(TEXT("vertexCount"));
				CompareNumField(TEXT("triangleCount"));
				CompareNumField(TEXT("lodCount"));
				CompareNumField(TEXT("sourceWidth"));
				CompareNumField(TEXT("sourceHeight"));
				CompareNumField(TEXT("width"));
				CompareNumField(TEXT("height"));
				CompareNumField(TEXT("mipCount"));
				CompareNumField(TEXT("parameterCount"));

				OutStructured->SetArrayField(TEXT("differences"), Differences);
				OutStructured->SetNumberField(TEXT("differenceCount"), Differences.Num());

				// Thumbnails
				if (bIncludeThumbnails)
				{
					TArray<TSharedPtr<FJsonValue>> ImageContent;

					TArray<uint8> PngA, PngB;
					FString ErrorA, ErrorB;
					bool bGotA = RenderThumbnail(AssetA, ThumbSize, ThumbSize, PngA, ErrorA);
					bool bGotB = RenderThumbnail(AssetB, ThumbSize, ThumbSize, PngB, ErrorB);

					if (bGotA) ImageContent.Add(MakeImageContentValue(PngA));
					if (bGotB) ImageContent.Add(MakeImageContentValue(PngB));

					if (!ImageContent.IsEmpty())
					{
						OutStructured->SetArrayField(TEXT("_imageContent"), ImageContent);
					}
					OutStructured->SetBoolField(TEXT("thumbnailA_available"), bGotA);
					OutStructured->SetBoolField(TEXT("thumbnailB_available"), bGotB);
					if (!bGotA) OutStructured->SetStringField(TEXT("thumbnailA_error"), ErrorA);
					if (!bGotB) OutStructured->SetStringField(TEXT("thumbnailB_error"), ErrorB);
				}

				OutSummary = FString::Printf(TEXT("Compared '%s' vs '%s' — %d differences found."),
					*AssetPathA, *AssetPathB, Differences.Num());
				return true;
			}
		});

		// ── 4. asset_batch_query ──
		// 批量查询资产列表的详细信息（文件大小、类型等），用于资产管理

		Registry.Register({
			TEXT("asset_batch_query"),
			TEXT("Query detailed information for multiple assets at once. Returns a table with name, class, file size, and type for each asset. Ideal for auditing content folders, finding large assets, or inventory management."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Full object path of the asset.")), TEXT("Array of asset object paths to query."))},
					{TEXT("include_details"), FSololmcpSchemaBuilder::Boolean(TEXT("Include detailed type-specific properties (vertices, resolution, etc.). Slower but more comprehensive. Default false."))},
					{TEXT("fail_on_missing"), FSololmcpSchemaBuilder::Boolean(TEXT("If true, return an error when any requested asset is missing. Default false."))}
				},
				{TEXT("asset_paths")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TArray<TSharedPtr<FJsonValue>>* PathArray = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("asset_paths"), PathArray) || !PathArray || PathArray->Num() == 0)
				{
					OutError = TEXT("Missing or empty argument: asset_paths");
					return false;
				}

				const bool bIncludeDetails = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_details"))
					? Arguments->GetBoolField(TEXT("include_details")) : false;
				const bool bFailOnMissing = Arguments->HasTypedField<EJson::Boolean>(TEXT("fail_on_missing"))
					? Arguments->GetBoolField(TEXT("fail_on_missing")) : false;

				int64 TotalSizeBytes = 0;
				int32 FoundCount = 0;
				int32 MissingCount = 0;
				TArray<TSharedPtr<FJsonValue>> Results;
				TArray<TSharedPtr<FJsonValue>> MissingAssets;

				for (const TSharedPtr<FJsonValue>& PathVal : *PathArray)
				{
					if (!PathVal.IsValid()) continue;
					FString AssetPath;
					if (!PathVal->TryGetString(AssetPath)) continue;

					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("assetPath"), AssetPath);

					int64 FileSize = GetAssetFileSize(AssetPath);
					TotalSizeBytes += FileSize;
					Entry->SetNumberField(TEXT("fileSizeBytes"), FileSize);

					// Get AssetRegistry data
					FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
					FAssetData AssetData;
					// UE 5.7: Use FSoftObjectPath version
					FSoftObjectPath SoftPath(AssetPath);
					AssetData = ARM.Get().GetAssetByObjectPath(SoftPath);
					if (AssetData.IsValid())
					{
						++FoundCount;
						Entry->SetBoolField(TEXT("exists"), true);
						Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
						Entry->SetStringField(TEXT("classPath"), AssetData.AssetClassPath.ToString());
						Entry->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());
					}
					else
					{
						++MissingCount;
						Entry->SetBoolField(TEXT("exists"), false);
						Entry->SetStringField(TEXT("name"), FPackageName::GetShortName(*AssetPath));
						Entry->SetStringField(TEXT("classPath"), TEXT("unknown"));
						Entry->SetStringField(TEXT("status"), TEXT("missing"));
						MissingAssets.Add(MakeShared<FJsonValueString>(AssetPath));
					}

					// Optionally load and extract details
					if (bIncludeDetails)
					{
						FString LoadError;
						UObject* Asset = LoadAssetChecked(AssetPath, LoadError);
						if (Asset)
						{
							TSharedRef<FJsonObject> Props = ExtractCommonProperties(Asset);
							Entry->SetObjectField(TEXT("properties"), Props);
						}
						else
						{
							Entry->SetStringField(TEXT("loadError"), LoadError);
						}
					}

					Results.Add(MakeShared<FJsonValueObject>(Entry));
				}

				OutStructured->SetArrayField(TEXT("assets"), Results);
				OutStructured->SetArrayField(TEXT("missing_assets"), MissingAssets);
				OutStructured->SetNumberField(TEXT("count"), Results.Num());
				OutStructured->SetNumberField(TEXT("found_count"), FoundCount);
				OutStructured->SetNumberField(TEXT("missing_count"), MissingCount);
				OutStructured->SetStringField(TEXT("status"),
					MissingCount == 0 ? TEXT("ok") : (FoundCount == 0 ? TEXT("all_missing") : TEXT("partial_missing")));
				OutStructured->SetNumberField(TEXT("totalSizeBytes"), TotalSizeBytes);
				// Format total size for readability
				OutStructured->SetStringField(TEXT("totalSizeFormatted"),
					FString::Printf(TEXT("%.2f MB"), static_cast<double>(TotalSizeBytes) / (1024.0 * 1024.0)));

				OutSummary = FString::Printf(TEXT("Queried %d assets, total size %.2f MB."),
					Results.Num(), static_cast<double>(TotalSizeBytes) / (1024.0 * 1024.0));
				if (bFailOnMissing && MissingCount > 0)
				{
					SololmcpError::Set(OutStructured, TEXT("MISSING_ASSET"), TEXT("asset_paths"),
						TEXT("One or more requested assets were not found."));
					OutError = FString::Printf(TEXT("%d requested assets were missing."), MissingCount);
					return false;
				}
				return true;
			}
		});

		// ── 5. asset_find_references ──
		// 查找资产的所有引用者和依赖项

		Registry.Register({
			TEXT("asset_find_references"),
			TEXT("Find all assets that reference (depend on) or are referenced by (dependencies of) a given asset. Essential for understanding asset relationships, finding broken references, or preparing for asset deletion (checking what would be affected)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Full object path of the asset."))},
					{TEXT("direction"), FSololmcpSchemaBuilder::String(TEXT("Reference direction: 'referencers' (assets that use this asset), 'dependencies' (assets this asset uses), or 'both'. Default 'both'."), {TEXT("referencers"), TEXT("dependencies"), TEXT("both")})},
					{TEXT("class_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional class path filter, e.g. '/Script/Engine.StaticMesh' to only show StaticMesh references."))},
					{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum number of results per direction. Default 100."))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				FString Direction;
				Arguments->TryGetStringField(TEXT("direction"), Direction);
				if (Direction.IsEmpty()) Direction = TEXT("both");

				FString ClassFilter;
				Arguments->TryGetStringField(TEXT("class_filter"), ClassFilter);

				int32 MaxResults = 100;
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
				MaxResults = FMath::Clamp(MaxResults, 1, 1000);

				// Get AssetRegistry data for the source asset
				FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = ARM.Get();

				auto NormalizeObjectPath = [](const FString& Path) -> FString
				{
					const int32 LastSlash = Path.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					const int32 LastDot = Path.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					if (LastDot > LastSlash)
					{
						return Path;
					}
					FString AssetName = Path;
					int32 SlashIndex = INDEX_NONE;
					if (AssetName.FindLastChar(TEXT('/'), SlashIndex))
					{
						AssetName = AssetName.Mid(SlashIndex + 1);
					}
					return FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
				};

				FAssetData SourceData;
				SourceData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NormalizeObjectPath(AssetPath)));
				if (!SourceData.IsValid())
				{
					TArray<FAssetData> PackageAssets;
					AssetRegistry.GetAssetsByPackageName(FName(*AssetPath), PackageAssets);
					if (PackageAssets.Num() > 0)
					{
						SourceData = PackageAssets[0];
					}
				}
				if (SourceData.IsValid())
				{
					OutStructured->SetObjectField(TEXT("sourceAsset"), AssetDataToJsonDetailed(SourceData));
				}
				else
				{
					OutStructured->SetBoolField(TEXT("source_found"), false);
					OutStructured->SetStringField(TEXT("status"), TEXT("source_not_found"));
					OutStructured->SetStringField(TEXT("assetPath"), AssetPath);
					OutError = FString::Printf(TEXT("Source asset not found: %s"), *AssetPath);
					return false;
				}
				OutStructured->SetStringField(TEXT("assetPath"), AssetPath);
				OutStructured->SetBoolField(TEXT("source_found"), true);

				// Build class filter
				FARFilter Filter;
				if (!ClassFilter.IsEmpty())
				{
					Filter.ClassPaths.Add(FTopLevelAssetPath(*ClassFilter));
				}

				int32 TotalCount = 0;

				// Referencers (who uses this asset)
				if (Direction == TEXT("referencers") || Direction == TEXT("both"))
				{
					TArray<FName> Referencers;
					GetReferencers(AssetPath, Referencers);

					TArray<TSharedPtr<FJsonValue>> RefsJson;
					for (int32 i = 0; i < FMath::Min(Referencers.Num(), MaxResults); ++i)
					{
								// UE 5.7: GetAssetByObjectPath FName deprecated, use FSoftObjectPath
						FAssetData RefData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Referencers[i].ToString()));
						if (RefData.IsValid())
						{
							if (ClassFilter.IsEmpty() || RefData.AssetClassPath.ToString().Contains(ClassFilter))
							{
								RefsJson.Add(MakeShared<FJsonValueObject>(AssetDataToJsonDetailed(RefData)));
							}
						}
						else
						{
							TSharedRef<FJsonObject> SimpleRef = MakeShared<FJsonObject>();
							SimpleRef->SetStringField(TEXT("objectPath"), Referencers[i].ToString());
							RefsJson.Add(MakeShared<FJsonValueObject>(SimpleRef));
						}
					}

					OutStructured->SetArrayField(TEXT("referencers"), RefsJson);
					OutStructured->SetNumberField(TEXT("referencerCount"), Referencers.Num());
					TotalCount += Referencers.Num();
				}

				// Dependencies (what this asset uses)
				if (Direction == TEXT("dependencies") || Direction == TEXT("both"))
				{
					TArray<FName> Dependencies;
					GetDependencies(AssetPath, Dependencies);

					TArray<TSharedPtr<FJsonValue>> DepsJson;
					for (int32 i = 0; i < FMath::Min(Dependencies.Num(), MaxResults); ++i)
					{
						// UE 5.7: GetAssetByObjectPath FName deprecated, use FSoftObjectPath
						FAssetData DepData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Dependencies[i].ToString()));
						if (DepData.IsValid())
						{
							if (ClassFilter.IsEmpty() || DepData.AssetClassPath.ToString().Contains(ClassFilter))
							{
								DepsJson.Add(MakeShared<FJsonValueObject>(AssetDataToJsonDetailed(DepData)));
							}
						}
						else
						{
							TSharedRef<FJsonObject> SimpleDep = MakeShared<FJsonObject>();
							SimpleDep->SetStringField(TEXT("objectPath"), Dependencies[i].ToString());
							DepsJson.Add(MakeShared<FJsonValueObject>(SimpleDep));
						}
					}

					OutStructured->SetArrayField(TEXT("dependencies"), DepsJson);
					OutStructured->SetNumberField(TEXT("dependencyCount"), Dependencies.Num());
					TotalCount += Dependencies.Num();
				}

				OutSummary = FString::Printf(TEXT("Found %d reference relationships for '%s' (direction: %s)."),
					TotalCount, *AssetPath, *Direction);
				OutStructured->SetStringField(TEXT("status"), TotalCount > 0 ? TEXT("ok") : TEXT("no_relationships"));
				OutStructured->SetNumberField(TEXT("relationship_count"), TotalCount);
				return true;
			}
		});

		// ── 6. asset_rename ──
		// 重命名资产（在现有 asset_rename 基础上提供更安全的版本，带引用更新通知）

		Registry.Register({
			TEXT("asset_rename_safe"),
			TEXT("Rename an asset with reference tracking. UE automatically updates internal references, and this tool reports what was updated. Use this instead of file-system renames to avoid broken references."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Current full object path of the asset."))},
					{TEXT("new_name"), FSololmcpSchemaBuilder::String(TEXT("New asset name (without path)."))}
				},
				{TEXT("asset_path"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing arguments: asset_path and new_name");
					return false;
				}

				// Build destination path (same directory, new name)
				FString PackagePath;
				FString OldName;
				AssetPath.Split(TEXT("/"), &PackagePath, &OldName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
				FString DestinationPath = PackagePath + TEXT("/") + NewName;
				if (OldName == NewName)
				{
					OutError = TEXT("New asset name matches the current name.");
					return false;
				}

				// Validate: check if destination already exists
				if (LoadObject<UObject>(nullptr, *DestinationPath, nullptr, LOAD_None, nullptr))
				{
					OutError = FString::Printf(TEXT("Destination asset already exists: '%s'"), *DestinationPath);
					return false;
				}

				// Find referencers before rename (for reporting)
				TArray<FName> ReferencersBefore;
				GetReferencers(AssetPath, ReferencersBefore);

				// Execute the rename
				if (!Context.Services.RenameAsset(AssetPath, DestinationPath, OutError))
				{
					return false;
				}
				UObject* RenamedAsset = LoadObject<UObject>(nullptr, *DestinationPath, nullptr, LOAD_None, nullptr);
				if (!RenamedAsset)
				{
					OutError = FString::Printf(TEXT("Rename verification failed: destination asset did not load: '%s'"), *DestinationPath);
					return false;
				}

				OutStructured->SetStringField(TEXT("oldPath"), AssetPath);
				OutStructured->SetStringField(TEXT("newPath"), DestinationPath);
				OutStructured->SetNumberField(TEXT("referencersUpdated"), ReferencersBefore.Num());

				// List referencers that were auto-updated
				TArray<TSharedPtr<FJsonValue>> RefsJson;
				for (const FName& Ref : ReferencersBefore)
				{
					RefsJson.Add(MakeShared<FJsonValueString>(Ref.ToString()));
				}
				OutStructured->SetArrayField(TEXT("autoUpdatedReferences"), RefsJson);

				OutSummary = FString::Printf(TEXT("Renamed '%s' to '%s'. %d references were auto-updated."),
					*AssetPath, *DestinationPath, ReferencersBefore.Num());
				return true;
			}
		});

		// ── 7. asset_smart_search ──
		// 智能资产搜索：支持中文/英文关键词、多 token 模糊匹配、语义类别映射
		// 返回格式: {results: [{path, name, class, score, thumbnail?}], count, totalScanned, _imageContent?}

		Registry.Register({
			TEXT("asset_smart_search"),
			TEXT("Search assets using natural language keywords with fuzzy matching. Supports Chinese/English terms, multiple tokens (space-separated), and semantic class mapping (e.g. 'cartoon tree', 'character', 'weapon'). Use this instead of asset_search for user-facing queries. Set include_thumbnails=true to embed PNG thumbnail images for each result."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("query"), FSololmcpSchemaBuilder::String(TEXT("Natural language search query, e.g. 'cartoon tree', 'character human', 'stone rock'"))},
					{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum results. Default 20."))},
					{TEXT("class_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional class filter: StaticMesh, SkeletalMesh, Material, Texture2D, etc."))},
					{TEXT("include_thumbnails"), FSololmcpSchemaBuilder::Boolean(TEXT("If true, render and embed PNG thumbnail for each result. Max 12 thumbnails. Default false."))},
					{TEXT("thumbnail_size"), FSololmcpSchemaBuilder::Integer(TEXT("Thumbnail max width/height in pixels. Default 128. Range 32-512."))}
				},
				{TEXT("query")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Query;
				if (!Arguments->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
				{
					OutError = TEXT("Missing argument: query");
					return false;
				}

				int32 MaxResults = 20;
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
				MaxResults = FMath::Clamp(MaxResults, 1, 100);

				FString ClassFilter;
				Arguments->TryGetStringField(TEXT("class_filter"), ClassFilter);

				bool bIncludeThumbnails = false;
				Arguments->TryGetBoolField(TEXT("include_thumbnails"), bIncludeThumbnails);

				int32 ThumbSize = 128;
				Arguments->TryGetNumberField(TEXT("thumbnail_size"), ThumbSize);
				ThumbSize = FMath::Clamp(ThumbSize, 32, 512);

				// ── Tokenize query ──
				// Split by space, comma, Chinese comma
				TArray<FString> RawTokens;
				Query.ParseIntoArray(RawTokens, TEXT(" "), true);
				// Also split by comma
				TArray<FString> Tokens;
				for (const FString& RT : RawTokens)
				{
					TArray<FString> SubTokens;
					RT.ParseIntoArray(SubTokens, TEXT(","), true);
					for (const FString& ST : SubTokens)
					{
						FString Trimmed = ST.TrimStartAndEnd();
						if (!Trimmed.IsEmpty()) Tokens.Add(Trimmed);
					}
				}

				if (Tokens.Num() == 0)
				{
					OutError = TEXT("Empty query after tokenization");
					return false;
				}

				// ── Expand tokens with synonyms ──
				// Chinese-English keyword map for asset search
				// User types "卡通树" → tokens ["卡通", "树"] → expand both to English equivalents
				TArray<FString> ExpandedTokens;
				auto ExpandToken = [&](const FString& Lower)
				{
					// tree / 树 / shu
					if (Lower == TEXT("tree") || Lower == TEXT("shu")
						|| Lower == TEXT("\u6811")) // 树
					{
						ExpandedTokens.Add(TEXT("tree"));
						ExpandedTokens.Add(TEXT("oak"));
						ExpandedTokens.Add(TEXT("pine"));
						ExpandedTokens.Add(TEXT("birch"));
						ExpandedTokens.Add(TEXT("willow"));
						ExpandedTokens.Add(TEXT("palm"));
						ExpandedTokens.Add(TEXT("bush"));
						ExpandedTokens.Add(TEXT("plant"));
						ExpandedTokens.Add(TEXT("foliage"));
					}
					// cartoon / 卡通 / katong
					else if (Lower == TEXT("cartoon") || Lower == TEXT("katong")
						|| Lower == TEXT("\u5361\u901a")) // 卡通
					{
						ExpandedTokens.Add(TEXT("cartoon"));
						ExpandedTokens.Add(TEXT("stylized"));
						ExpandedTokens.Add(TEXT("toon"));
						ExpandedTokens.Add(TEXT("lowpoly"));
						ExpandedTokens.Add(TEXT("cute"));
						ExpandedTokens.Add(TEXT("simple"));
					}
					// character/human/person / 角色/人/人物 / juese/ren
					else if (Lower == TEXT("character") || Lower == TEXT("human") || Lower == TEXT("person")
						|| Lower == TEXT("juese") || Lower == TEXT("ren")
						|| Lower == TEXT("\u89d2\u8272") || Lower == TEXT("\u4eba") || Lower == TEXT("\u4eba\u7269")) // 角色/人/人物
					{
						ExpandedTokens.Add(TEXT("character"));
						ExpandedTokens.Add(TEXT("human"));
						ExpandedTokens.Add(TEXT("person"));
						ExpandedTokens.Add(TEXT("npc"));
						ExpandedTokens.Add(TEXT("man"));
						ExpandedTokens.Add(TEXT("woman"));
						ExpandedTokens.Add(TEXT("girl"));
						ExpandedTokens.Add(TEXT("boy"));
						ExpandedTokens.Add(TEXT("hero"));
						ExpandedTokens.Add(TEXT("enemy"));
					}
					// weapon / 武器 / wuqi
					else if (Lower == TEXT("weapon") || Lower == TEXT("wuqi")
						|| Lower == TEXT("\u6b66\u5668")) // 武器
					{
						ExpandedTokens.Add(TEXT("weapon"));
						ExpandedTokens.Add(TEXT("sword"));
						ExpandedTokens.Add(TEXT("gun"));
						ExpandedTokens.Add(TEXT("rifle"));
						ExpandedTokens.Add(TEXT("axe"));
						ExpandedTokens.Add(TEXT("bow"));
						ExpandedTokens.Add(TEXT("shield"));
					}
					// building/house / 建筑/房子 / fangzi
					else if (Lower == TEXT("building") || Lower == TEXT("fangzi") || Lower == TEXT("house")
						|| Lower == TEXT("\u5efa\u7b51") || Lower == TEXT("\u623f\u5b50")) // 建筑/房子
					{
						ExpandedTokens.Add(TEXT("building"));
						ExpandedTokens.Add(TEXT("house"));
						ExpandedTokens.Add(TEXT("cottage"));
						ExpandedTokens.Add(TEXT("tower"));
						ExpandedTokens.Add(TEXT("castle"));
						ExpandedTokens.Add(TEXT("wall"));
						ExpandedTokens.Add(TEXT("ruin"));
					}
					// rock/stone / 石头/岩石 / shitou
					else if (Lower == TEXT("rock") || Lower == TEXT("stone") || Lower == TEXT("shitou")
						|| Lower == TEXT("\u77f3\u5934") || Lower == TEXT("\u5ca9\u77f3")) // 石头/岩石
					{
						ExpandedTokens.Add(TEXT("rock"));
						ExpandedTokens.Add(TEXT("stone"));
						ExpandedTokens.Add(TEXT("boulder"));
						ExpandedTokens.Add(TEXT("pebble"));
					}
					// vehicle/car / 车/车辆 / che
					else if (Lower == TEXT("vehicle") || Lower == TEXT("che") || Lower == TEXT("car")
						|| Lower == TEXT("\u8f66") || Lower == TEXT("\u8f66\u8f86")) // 车/车辆
					{
						ExpandedTokens.Add(TEXT("vehicle"));
						ExpandedTokens.Add(TEXT("car"));
						ExpandedTokens.Add(TEXT("truck"));
						ExpandedTokens.Add(TEXT("tank"));
					}
					// terrain/ground / 地形/地面
					else if (Lower == TEXT("terrain") || Lower == TEXT("ground")
						|| Lower == TEXT("\u5730\u5f62") || Lower == TEXT("\u5730\u9762")) // 地形/地面
					{
						ExpandedTokens.Add(TEXT("terrain"));
						ExpandedTokens.Add(TEXT("landscape"));
						ExpandedTokens.Add(TEXT("ground"));
					}
					// grass/grassland / 草/草地
					else if (Lower == TEXT("grass") || Lower == TEXT("grassland")
						|| Lower == TEXT("\u8349") || Lower == TEXT("\u8349\u5730")) // 草/草地
					{
						ExpandedTokens.Add(TEXT("grass"));
						ExpandedTokens.Add(TEXT("grassland"));
						ExpandedTokens.Add(TEXT("lawn"));
					}
					// water/river / 水/河流
					else if (Lower == TEXT("water") || Lower == TEXT("river")
						|| Lower == TEXT("\u6c34") || Lower == TEXT("\u6cb3\u6d41")) // 水/河流
					{
						ExpandedTokens.Add(TEXT("water"));
						ExpandedTokens.Add(TEXT("river"));
						ExpandedTokens.Add(TEXT("lake"));
						ExpandedTokens.Add(TEXT("ocean"));
						ExpandedTokens.Add(TEXT("stream"));
					}
				};

				for (const FString& Token : Tokens)
				{
					ExpandedTokens.Add(Token.ToLower());
					ExpandToken(Token.ToLower());
				}

				// ── Semantic class inference ──
				TSet<FString> ClassHints;
				for (const FString& Token : Tokens)
				{
					FString L = Token.ToLower();
					// character / 角色 / 人 / 人物
					if (L == TEXT("character") || L == TEXT("human") || L == TEXT("person")
						|| L == TEXT("juese") || L == TEXT("ren")
						|| L == TEXT("\u89d2\u8272") || L == TEXT("\u4eba") || L == TEXT("\u4eba\u7269")
						|| L == TEXT("npc"))
					{
						ClassHints.Add(TEXT("SkeletalMesh"));
					}
					// static mesh types: tree/rock/building/weapon/vehicle / 树/石头/建筑/武器/车
					else if (L == TEXT("tree") || L == TEXT("shu") || L == TEXT("\u6811")
						|| L == TEXT("rock") || L == TEXT("stone") || L == TEXT("shitou")
						|| L == TEXT("\u77f3\u5934") || L == TEXT("\u5ca9\u77f3")
						|| L == TEXT("building") || L == TEXT("fangzi") || L == TEXT("house")
						|| L == TEXT("\u5efa\u7b51") || L == TEXT("\u623f\u5b50")
						|| L == TEXT("weapon") || L == TEXT("wuqi") || L == TEXT("\u6b66\u5668")
						|| L == TEXT("vehicle") || L == TEXT("che") || L == TEXT("car")
						|| L == TEXT("\u8f66") || L == TEXT("\u8f66\u8f86")
						|| L == TEXT("prop"))
					{
						ClassHints.Add(TEXT("StaticMesh"));
					}
					// texture/material / 贴图/材质
					else if (L == TEXT("texture") || L == TEXT("material")
						|| L == TEXT("\u8d34\u56fe") || L == TEXT("\u6750\u8d28")) // 贴图/材质
					{
						ClassHints.Add(TEXT("Texture2D"));
						ClassHints.Add(TEXT("Material"));
						ClassHints.Add(TEXT("MaterialInstance"));
					}
					// animation / 动画
					else if (L == TEXT("animation") || L == TEXT("anim")
						|| L == TEXT("\u52a8\u753b")) // 动画
					{
						ClassHints.Add(TEXT("AnimSequence"));
						ClassHints.Add(TEXT("AnimMontage"));
					}
				}

				// ── AssetRegistry scan ──
				FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				FARFilter Filter;
				Filter.bRecursivePaths = true;
				Filter.PackagePaths.Add(FName(TEXT("/Game")));

				// Apply explicit class filter
				if (!ClassFilter.IsEmpty())
				{
					// Try known mappings
					TMap<FString, FString> ClassNameMap;
					ClassNameMap.Add(TEXT("StaticMesh"), TEXT("/Script/Engine.StaticMesh"));
					ClassNameMap.Add(TEXT("SkeletalMesh"), TEXT("/Script/Engine.SkeletalMesh"));
					ClassNameMap.Add(TEXT("Material"), TEXT("/Script/Engine.Material"));
					ClassNameMap.Add(TEXT("MaterialInstance"), TEXT("/Script/Engine.MaterialInstance"));
					ClassNameMap.Add(TEXT("Texture2D"), TEXT("/Script/Engine.Texture2D"));
					ClassNameMap.Add(TEXT("AnimSequence"), TEXT("/Script/Engine.AnimSequence"));
					ClassNameMap.Add(TEXT("Blueprint"), TEXT("/Script/Engine.Blueprint"));
					ClassNameMap.Add(TEXT("SoundWave"), TEXT("/Script/Engine.SoundWave"));
					ClassNameMap.Add(TEXT("NiagaraSystem"), TEXT("/Script/Niagara.NiagaraSystem"));

					FString* FoundClass = ClassNameMap.Find(ClassFilter);
					if (FoundClass)
					{
						Filter.ClassPaths.Add(FTopLevelAssetPath(*(*FoundClass)));
					}
				}

				TArray<FAssetData> AllAssets;
				ARM.Get().GetAssets(Filter, AllAssets);

				// ── Score & rank assets ──
				struct FScoredAsset
				{
					FAssetData AssetData;
					int32 Score;
				};
				TArray<FScoredAsset> Scored;

				for (const FAssetData& A : AllAssets)
				{
					int32 Score = 0;
					FString AssetNameStr = A.AssetName.ToString().ToLower();
					FString PathStr = A.GetObjectPathString().ToLower();
					FString ClassStr = A.AssetClassPath.GetAssetName().ToString().ToLower();

					// Check class hints
					if (ClassHints.Num() > 0)
					{
						bool bClassMatch = false;
						for (const FString& Hint : ClassHints)
						{
							if (ClassStr.Contains(Hint.ToLower()))
							{
								bClassMatch = true;
								break;
							}
						}
						if (bClassMatch) Score += 3;
					}

					// Check expanded tokens against name and path
					// Original tokens get higher weight
					for (const FString& Token : Tokens)
					{
						FString LT = Token.ToLower();
						if (AssetNameStr.Contains(LT)) Score += 10; // Exact original token in name
						else if (PathStr.Contains(LT)) Score += 5;  // In path
					}

					// Expanded synonyms get lower weight
					for (const FString& Token : ExpandedTokens)
					{
						if (Tokens.Contains(Token)) continue; // Already scored above
						if (AssetNameStr.Contains(Token)) Score += 4;
						else if (PathStr.Contains(Token)) Score += 2;
					}

					if (Score > 0)
					{
						Scored.Add({A, Score});
					}
				}

				// Sort by score descending
				Scored.Sort([](const FScoredAsset& A, const FScoredAsset& B)
				{
					return A.Score > B.Score;
				});

				// Build results
				TArray<TSharedPtr<FJsonValue>> Results;
				for (int32 i = 0; i < FMath::Min(Scored.Num(), MaxResults); ++i)
				{
					const FAssetData& A = Scored[i].AssetData;
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("path"), A.GetObjectPathString());
					O->SetStringField(TEXT("name"), A.AssetName.ToString());
					O->SetStringField(TEXT("class"), A.AssetClassPath.GetAssetName().ToString());
					O->SetNumberField(TEXT("score"), Scored[i].Score);
					Results.Add(MakeShared<FJsonValueObject>(O));
				}

				OutStructured->SetArrayField(TEXT("results"), Results);
				OutStructured->SetNumberField(TEXT("count"), Results.Num());
				OutStructured->SetNumberField(TEXT("totalScanned"), AllAssets.Num());

				// ── Optional: render thumbnails ──
				if (bIncludeThumbnails && Results.Num() > 0)
				{
					const int32 MaxThumbs = FMath::Min(Results.Num(), 12);
					TArray<TSharedPtr<FJsonValue>> ImageContent;

					for (int32 i = 0; i < MaxThumbs; ++i)
					{
						// Get path from result object
						FString AssetPath;
						if (!Results[i]->AsObject()->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
							continue;

						// Load asset for thumbnail rendering
						UObject* AssetObj = LoadObject<UObject>(nullptr, *AssetPath);
						if (!AssetObj) continue;

						TArray<uint8> PngData;
						FString ThumbError;
						if (RenderThumbnail(AssetObj, ThumbSize, ThumbSize, PngData, ThumbError))
						{
							ImageContent.Add(MakeImageContentValue(PngData));
							// Also mark the result object with thumbnail index
							Results[i]->AsObject()->SetNumberField(TEXT("thumbnail_index"), ImageContent.Num() - 1);
						}
					}

					if (ImageContent.Num() > 0)
					{
						OutStructured->SetArrayField(TEXT("_imageContent"), ImageContent);
					}
				}

				OutSummary = FString::Printf(TEXT("Smart search '%s': %d results (from %d assets)%s"),
					*Query, Results.Num(), AllAssets.Num(),
					bIncludeThumbnails ? TEXT(" with thumbnails") : TEXT(""));
				return true;
			}
		});

		// P2 promotion batch A: concrete read-only asset/editor scripting tools.

		Registry.Register({
			TEXT("asset_metadata_get_v2"),
			TEXT("Concrete P2 asset metadata reader. Resolves an asset through the Asset Registry and returns object path, package, class path, tags, family, and load/dirty hints. Accepts asset_path or target_asset."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path, e.g. '/Game/Foo/SM_Bar' or '/Game/Foo/SM_Bar.SM_Bar'."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("include_tags"), FSololmcpSchemaBuilder::Boolean(TEXT("Include Asset Registry tags. Default true."))},
					{TEXT("include_object_metadata"), FSololmcpSchemaBuilder::Boolean(TEXT("Include UPackage/FMetaData object metadata. Default true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}

				bool bIncludeTags = true;
				Arguments->TryGetBoolField(TEXT("include_tags"), bIncludeTags);
				bool bIncludeObjectMetadata = true;
				Arguments->TryGetBoolField(TEXT("include_object_metadata"), bIncludeObjectMetadata);

				TSharedRef<FJsonObject> AssetJson = AssetDataToJsonDetailed(AssetData);
				if (!bIncludeTags)
				{
					AssetJson->RemoveField(TEXT("tags"));
				}

				UObject* LoadedAsset = AssetData.GetAsset();
				UPackage* Package = LoadedAsset ? LoadedAsset->GetOutermost() : FindPackage(nullptr, *AssetData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("status"), TEXT("ok"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetStringField(TEXT("resolvedObjectPath"), AssetData.GetObjectPathString());
				OutStructured->SetObjectField(TEXT("asset"), AssetJson);
				OutStructured->SetStringField(TEXT("family"), AssetFamilyFromObject(LoadedAsset, AssetData));
				OutStructured->SetBoolField(TEXT("loaded"), LoadedAsset != nullptr);
				OutStructured->SetBoolField(TEXT("dirty"), Package ? Package->IsDirty() : false);
				if (bIncludeObjectMetadata)
				{
					OutStructured->SetObjectField(TEXT("objectMetadata"), ObjectMetadataToJson(LoadedAsset));
				}
				OutStructured->SetStringField(TEXT("metadata_source"), bIncludeObjectMetadata ? TEXT("AssetRegistry.TagsAndValues + UPackage.FMetaData") : TEXT("AssetRegistry.TagsAndValues"));
				OutSummary = FString::Printf(TEXT("Resolved metadata for '%s' as '%s'."), *RequestedPath, *AssetData.GetObjectPathString());
				return true;
			},
			nullptr,
			0
		});

		Registry.Register({
			TEXT("asset_metadata_set_v2"),
			TEXT("Concrete P2 asset metadata writer. Defaults to dry-run; set execute=true to set/remove UPackage.FMetaData entries on an asset and optionally save."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("metadata"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Key/value metadata object. Values may be string, number, boolean, or null."))},
					{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Single metadata key alternative to metadata object."))},
					{TEXT("value"), FSololmcpSchemaBuilder::String(TEXT("Single metadata value alternative to metadata object."))},
					{TEXT("remove_keys"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Metadata keys to remove.")))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, only reports the patch plan."))},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after metadata mutation. Default true when execute=true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}

				TMap<FName, FString> SetValues;
				TArray<FName> RemoveKeys;
				if (!ParseMetadataPatchArguments(Arguments, SetValues, RemoveKeys, OutError))
				{
					return false;
				}

				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bSaveAsset = true;
				Arguments->TryGetBoolField(TEXT("save_asset"), bSaveAsset);

				UObject* LoadedAsset = AssetData.GetAsset();
				if (!LoadedAsset)
				{
					OutError = FString::Printf(TEXT("Failed to load asset for metadata edit: '%s'."), *AssetData.GetObjectPathString());
					return false;
				}
				UPackage* Package = LoadedAsset->GetOutermost();
				if (!Package)
				{
					OutError = TEXT("Loaded asset has no package.");
					return false;
				}

				FSomolObjectMetadata& Metadata = GetPackageObjectMetadata(Package);
				const bool bDirtyBefore = Package->IsDirty();
				OutStructured->SetStringField(TEXT("operation"), TEXT("metadata_set"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetStringField(TEXT("assetPath"), AssetData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("objectPath"), LoadedAsset->GetPathName());
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("saveAsset"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);
				OutStructured->SetArrayField(TEXT("patch"), MetadataPatchRows(Metadata, LoadedAsset, SetValues, RemoveKeys));
				OutStructured->SetObjectField(TEXT("metadataBefore"), ObjectMetadataToJson(LoadedAsset));

				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run metadata patch for '%s': set %d, remove %d."),
						*AssetData.PackageName.ToString(), SetValues.Num(), RemoveKeys.Num());
					return true;
				}

				ApplyMetadataPatch(Metadata, LoadedAsset, SetValues, RemoveKeys);
				LoadedAsset->MarkPackageDirty();

				bool bSaved = false;
				if (bSaveAsset)
				{
					if (!Context.Services.SaveAsset(AssetData.PackageName.ToString(), false, OutError))
					{
						return false;
					}
					bSaved = true;
				}

				OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
				OutStructured->SetBoolField(TEXT("saved"), bSaved);
				OutStructured->SetBoolField(TEXT("dirtyAfter"), Package->IsDirty());
				OutStructured->SetObjectField(TEXT("metadataAfter"), ObjectMetadataToJson(LoadedAsset));
				OutSummary = FString::Printf(TEXT("Metadata patch applied to '%s': set %d, remove %d."),
					*AssetData.PackageName.ToString(), SetValues.Num(), RemoveKeys.Num());
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_metadata_bulk_apply"),
			TEXT("Concrete P2 bulk asset metadata writer. Defaults to dry-run; set execute=true to apply per-asset metadata patches and optionally save."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("items"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
						{TEXT("metadata"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Key/value metadata object."))},
						{TEXT("remove_keys"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}
					}, {TEXT("asset_path")}))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, only reports the patch plan."))},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save each mutated asset. Default true when execute=true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("items"), Items) || !Items || Items->Num() == 0)
				{
					OutError = TEXT("Missing or empty items array.");
					return false;
				}

				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bSaveAsset = true;
				Arguments->TryGetBoolField(TEXT("save_asset"), bSaveAsset);
				TArray<TSharedPtr<FJsonValue>> Rows;
				int32 ReadyCount = 0;
				int32 MutatedCount = 0;
				int32 FailedCount = 0;

				for (int32 Index = 0; Index < Items->Num(); ++Index)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetNumberField(TEXT("index"), Index);
					const TSharedPtr<FJsonObject> ItemObject = (*Items)[Index].IsValid() ? (*Items)[Index]->AsObject() : nullptr;
					if (!ItemObject.IsValid())
					{
						Row->SetStringField(TEXT("status"), TEXT("failed"));
						Row->SetStringField(TEXT("error"), TEXT("item is not an object."));
						Rows.Add(MakeShared<FJsonValueObject>(Row));
						FailedCount++;
						continue;
					}

					FString ItemError;
					FString AssetPath;
					if (!TryGetAssetPathArgument(ItemObject.ToSharedRef(), AssetPath, ItemError))
					{
						Row->SetStringField(TEXT("status"), TEXT("failed"));
						Row->SetStringField(TEXT("error"), ItemError);
						Rows.Add(MakeShared<FJsonValueObject>(Row));
						FailedCount++;
						continue;
					}

					TMap<FName, FString> SetValues;
					TArray<FName> RemoveKeys;
					if (!ParseMetadataPatchArguments(ItemObject.ToSharedRef(), SetValues, RemoveKeys, ItemError))
					{
						Row->SetStringField(TEXT("assetPath"), AssetPath);
						Row->SetStringField(TEXT("status"), TEXT("failed"));
						Row->SetStringField(TEXT("error"), ItemError);
						Rows.Add(MakeShared<FJsonValueObject>(Row));
						FailedCount++;
						continue;
					}

					FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
					IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
					FAssetData AssetData = ResolveAssetDataByPath(AssetRegistry, AssetPath);
					if (!AssetData.IsValid())
					{
						Row->SetStringField(TEXT("assetPath"), AssetPath);
						Row->SetStringField(TEXT("status"), TEXT("failed"));
						Row->SetStringField(TEXT("error"), TEXT("asset_not_found"));
						Rows.Add(MakeShared<FJsonValueObject>(Row));
						FailedCount++;
						continue;
					}

					UObject* LoadedAsset = AssetData.GetAsset();
					UPackage* Package = LoadedAsset ? LoadedAsset->GetOutermost() : nullptr;
					if (!LoadedAsset || !Package)
					{
						Row->SetStringField(TEXT("assetPath"), AssetPath);
						Row->SetStringField(TEXT("status"), TEXT("failed"));
						Row->SetStringField(TEXT("error"), TEXT("asset_load_failed"));
						Rows.Add(MakeShared<FJsonValueObject>(Row));
						FailedCount++;
						continue;
					}

					FSomolObjectMetadata& Metadata = GetPackageObjectMetadata(Package);
					Row->SetStringField(TEXT("assetPath"), AssetData.PackageName.ToString());
					Row->SetStringField(TEXT("objectPath"), LoadedAsset->GetPathName());
					Row->SetBoolField(TEXT("dirtyBefore"), Package->IsDirty());
					Row->SetArrayField(TEXT("patch"), MetadataPatchRows(Metadata, LoadedAsset, SetValues, RemoveKeys));
					ReadyCount++;

					if (!bExecute)
					{
						Row->SetStringField(TEXT("status"), TEXT("dry_run"));
						Rows.Add(MakeShared<FJsonValueObject>(Row));
						continue;
					}

					ApplyMetadataPatch(Metadata, LoadedAsset, SetValues, RemoveKeys);
					LoadedAsset->MarkPackageDirty();

					bool bSaved = false;
					if (bSaveAsset)
					{
						FString SaveError;
						if (!Context.Services.SaveAsset(AssetData.PackageName.ToString(), false, SaveError))
						{
							Row->SetStringField(TEXT("status"), TEXT("failed"));
							Row->SetStringField(TEXT("error"), SaveError);
							Rows.Add(MakeShared<FJsonValueObject>(Row));
							FailedCount++;
							continue;
						}
						bSaved = true;
					}

					Row->SetStringField(TEXT("status"), TEXT("completed"));
					Row->SetBoolField(TEXT("saved"), bSaved);
					Row->SetBoolField(TEXT("dirtyAfter"), Package->IsDirty());
					Row->SetObjectField(TEXT("metadataAfter"), ObjectMetadataToJson(LoadedAsset));
					Rows.Add(MakeShared<FJsonValueObject>(Row));
					MutatedCount++;
				}

				OutStructured->SetStringField(TEXT("operation"), TEXT("metadata_bulk_apply"));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("saveAsset"), bSaveAsset);
				OutStructured->SetNumberField(TEXT("itemCount"), Items->Num());
				OutStructured->SetNumberField(TEXT("readyCount"), ReadyCount);
				OutStructured->SetNumberField(TEXT("mutatedCount"), MutatedCount);
				OutStructured->SetNumberField(TEXT("failedCount"), FailedCount);
				OutStructured->SetArrayField(TEXT("items"), Rows);
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), FailedCount == 0 ? TEXT("dry_run") : TEXT("dry_run_with_failures"));
					OutStructured->SetBoolField(TEXT("requires_execute"), ReadyCount > 0);
				}
				else
				{
					OutStructured->SetStringField(TEXT("status"), FailedCount == 0 ? TEXT("completed") : TEXT("partial"));
				}
				OutSummary = FString::Printf(TEXT("Bulk metadata %s: %d items, %d ready, %d mutated, %d failed."),
					bExecute ? TEXT("apply") : TEXT("dry-run"), Items->Num(), ReadyCount, MutatedCount, FailedCount);
				return FailedCount == 0 || ReadyCount > 0;
			}
		});

		Registry.Register({
			TEXT("asset_dependency_graph_v2"),
			TEXT("Concrete P2 dependency graph reader. Returns Asset Registry dependencies and optional referencers for an asset, with package rows resolved back to asset data when possible."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum rows per relationship list. Default 200."))},
					{TEXT("include_referencers"), FSololmcpSchemaBuilder::Boolean(TEXT("Also include assets/packages that reference this asset. Default true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}

				int32 MaxResults = 200;
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
				MaxResults = FMath::Clamp(MaxResults, 1, 1000);
				bool bIncludeReferencers = true;
				Arguments->TryGetBoolField(TEXT("include_referencers"), bIncludeReferencers);

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				const FName PackageName = AssetData.PackageName;

				TArray<FName> Dependencies;
				AssetRegistry.GetDependencies(PackageName, Dependencies);
				TArray<FName> Referencers;
				if (bIncludeReferencers)
				{
					AssetRegistry.GetReferencers(PackageName, Referencers);
				}

				OutStructured->SetStringField(TEXT("status"), TEXT("ok"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetObjectField(TEXT("rootAsset"), AssetDataToJsonDetailed(AssetData));
				OutStructured->SetStringField(TEXT("rootPackageName"), PackageName.ToString());
				OutStructured->SetArrayField(TEXT("dependencyPackages"), NameListToJsonArray(Dependencies, MaxResults));
				OutStructured->SetArrayField(TEXT("dependencies"), PackageNameRowsToJson(AssetRegistry, Dependencies, MaxResults));
				OutStructured->SetNumberField(TEXT("dependencyCount"), Dependencies.Num());
				if (bIncludeReferencers)
				{
					OutStructured->SetArrayField(TEXT("referencerPackages"), NameListToJsonArray(Referencers, MaxResults));
					OutStructured->SetArrayField(TEXT("referencers"), PackageNameRowsToJson(AssetRegistry, Referencers, MaxResults));
					OutStructured->SetNumberField(TEXT("referencerCount"), Referencers.Num());
				}
				OutStructured->SetBoolField(TEXT("truncated"), Dependencies.Num() > MaxResults || Referencers.Num() > MaxResults);
				OutSummary = FString::Printf(TEXT("Dependency graph for '%s': %d dependencies%s."),
					*AssetData.GetObjectPathString(),
					Dependencies.Num(),
					bIncludeReferencers ? *FString::Printf(TEXT(", %d referencers"), Referencers.Num()) : TEXT(""));
				return true;
			},
			nullptr,
			30
		});

		Registry.Register({
			TEXT("asset_referencers_list_v2"),
			TEXT("Concrete P2 referencer reader. Returns packages and resolved assets that reference the requested asset."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum referencer rows. Default 200."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}

				int32 MaxResults = 200;
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
				MaxResults = FMath::Clamp(MaxResults, 1, 1000);

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				TArray<FName> Referencers;
				AssetRegistry.GetReferencers(AssetData.PackageName, Referencers);

				OutStructured->SetStringField(TEXT("status"), TEXT("ok"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetObjectField(TEXT("rootAsset"), AssetDataToJsonDetailed(AssetData));
				OutStructured->SetStringField(TEXT("rootPackageName"), AssetData.PackageName.ToString());
				OutStructured->SetArrayField(TEXT("referencerPackages"), NameListToJsonArray(Referencers, MaxResults));
				OutStructured->SetArrayField(TEXT("referencers"), PackageNameRowsToJson(AssetRegistry, Referencers, MaxResults));
				OutStructured->SetNumberField(TEXT("referencerCount"), Referencers.Num());
				OutStructured->SetBoolField(TEXT("truncated"), Referencers.Num() > MaxResults);
				OutSummary = FString::Printf(TEXT("Found %d referencers for '%s'."), Referencers.Num(), *AssetData.GetObjectPathString());
				return true;
			},
			nullptr,
			30
		});

		Registry.Register({
			TEXT("asset_editor_dirty_state"),
			TEXT("Concrete P2 asset dirty-state reader. Resolves an asset and reports whether its loaded package is dirty, so scheduler/QA lanes can decide whether a save or rollback gate is required."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}

				UObject* LoadedAsset = AssetData.GetAsset();
				UPackage* Package = LoadedAsset ? LoadedAsset->GetOutermost() : FindPackage(nullptr, *AssetData.PackageName.ToString());
				const bool bDirty = Package ? Package->IsDirty() : false;

				OutStructured->SetStringField(TEXT("status"), TEXT("ok"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetStringField(TEXT("resolvedObjectPath"), AssetData.GetObjectPathString());
				OutStructured->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
				OutStructured->SetBoolField(TEXT("loaded"), LoadedAsset != nullptr);
				OutStructured->SetBoolField(TEXT("packageLoaded"), Package != nullptr);
				OutStructured->SetBoolField(TEXT("dirty"), bDirty);
				OutSummary = FString::Printf(TEXT("Asset '%s' dirty state: %s."), *AssetData.GetObjectPathString(), bDirty ? TEXT("dirty") : TEXT("clean"));
				return true;
			},
			nullptr,
			10
		});

		Registry.Register({
			TEXT("editor_subsystem_catalog"),
			TEXT("Concrete P2 editor subsystem catalog. Returns a deterministic catalog of editor subsystems/modules useful to MCP planning, with availability probes for the running editor."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<TSharedPtr<FJsonValue>> Rows;
				Rows.Add(MakeShared<FJsonValueObject>(EditorSubsystemCatalogRow(TEXT("EditorAssetSubsystem"), TEXT("/Script/UnrealEd.EditorAssetSubsystem"), TEXT("asset load/save/duplicate/rename/delete through editor scripting"), TEXT("Primary asset scripting subsystem."))));
				Rows.Add(MakeShared<FJsonValueObject>(EditorSubsystemCatalogRow(TEXT("EditorActorSubsystem"), TEXT("/Script/UnrealEd.EditorActorSubsystem"), TEXT("level actor select/spawn/delete/query"), TEXT("Useful for multi-agent level authoring lanes."))));
				Rows.Add(MakeShared<FJsonValueObject>(EditorSubsystemCatalogRow(TEXT("UnrealEditorSubsystem"), TEXT("/Script/UnrealEd.UnrealEditorSubsystem"), TEXT("editor world and high-level editor context"), TEXT("Gateway for editor world dependent operations."))));
				Rows.Add(MakeShared<FJsonValueObject>(EditorSubsystemCatalogRow(TEXT("LevelEditorSubsystem"), TEXT("/Script/LevelEditor.LevelEditorSubsystem"), TEXT("level editor viewport and level operations"), TEXT("Availability can vary with editor modules."))));
				Rows.Add(MakeShared<FJsonValueObject>(EditorSubsystemCatalogRow(TEXT("AssetEditorSubsystem"), TEXT("/Script/UnrealEd.AssetEditorSubsystem"), TEXT("open/focus/close asset editors"), TEXT("Used by asset editor queue lanes."))));
				Rows.Add(MakeShared<FJsonValueObject>(EditorSubsystemCatalogRow(TEXT("ImportSubsystem"), TEXT("/Script/UnrealEd.ImportSubsystem"), TEXT("import task hooks and import notifications"), TEXT("Complements AssetTools import task execution."))));

				TSharedRef<FJsonObject> Modules = MakeShared<FJsonObject>();
				Modules->SetBoolField(TEXT("UnrealEd"), FModuleManager::Get().IsModuleLoaded(TEXT("UnrealEd")));
				Modules->SetBoolField(TEXT("AssetRegistry"), FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")));
				Modules->SetBoolField(TEXT("AssetTools"), FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools")));
				Modules->SetBoolField(TEXT("LevelEditor"), FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")));
				Modules->SetBoolField(TEXT("EditorScriptingUtilities"), FModuleManager::Get().IsModuleLoaded(TEXT("EditorScriptingUtilities")));

				OutStructured->SetStringField(TEXT("status"), TEXT("ok"));
				OutStructured->SetStringField(TEXT("scope"), TEXT("editor_subsystems_asset_scripting"));
				OutStructured->SetArrayField(TEXT("subsystems"), Rows);
				OutStructured->SetObjectField(TEXT("modules"), Modules);
				OutStructured->SetStringField(TEXT("version_gate"), TEXT("5.7+"));
				OutSummary = FString::Printf(TEXT("Cataloged %d editor subsystem probes for MCP planning."), Rows.Num());
				return true;
			},
			nullptr,
			60
		});

		Registry.Register({
			TEXT("asset_consolidate_plan"),
			TEXT("Concrete P2 asset consolidate planner. Read-only impact plan for replacing source assets with a target asset; does not mutate assets."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("source_assets"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Source assets to consolidate away.")), TEXT("Source assets to consolidate away."))},
					{TEXT("from_assets"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Alias for source_assets.")), TEXT("Alias for source_assets."))},
					{TEXT("source_asset"), FSololmcpSchemaBuilder::String(TEXT("Single source asset alternative."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target asset to keep."))},
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for target_asset."))},
					{TEXT("max_referencers"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum referencer rows per source. Default 50."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> SourcePaths;
				if (!TryGetStringArrayArgument(Arguments, TEXT("source_assets"), SourcePaths))
				{
					TryGetStringArrayArgument(Arguments, TEXT("from_assets"), SourcePaths);
				}
				FString SingleSource;
				if (Arguments->TryGetStringField(TEXT("source_asset"), SingleSource))
				{
					SingleSource.TrimStartAndEndInline();
					if (!SingleSource.IsEmpty())
					{
						SourcePaths.Add(SingleSource);
					}
				}
				if (SourcePaths.Num() == 0)
				{
					OutError = TEXT("Missing source assets. Provide source_assets/from_assets or source_asset.");
					return false;
				}

				FString TargetPath;
				if (!Arguments->TryGetStringField(TEXT("target_asset"), TargetPath))
				{
					Arguments->TryGetStringField(TEXT("asset_path"), TargetPath);
				}
				TargetPath.TrimStartAndEndInline();
				if (TargetPath.IsEmpty())
				{
					OutError = TEXT("Missing target_asset (or asset_path).");
					return false;
				}

				int32 MaxReferencers = 50;
				Arguments->TryGetNumberField(TEXT("max_referencers"), MaxReferencers);
				MaxReferencers = FMath::Clamp(MaxReferencers, 0, 250);

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				FAssetData TargetData = ResolveAssetDataByPath(AssetRegistry, TargetPath);
				if (!TargetData.IsValid())
				{
					OutError = FString::Printf(TEXT("Target asset not found: '%s'."), *TargetPath);
					return false;
				}
				UObject* TargetObject = TargetData.GetAsset();
				const FString TargetFamily = AssetFamilyFromObject(TargetObject, TargetData);

				TArray<TSharedPtr<FJsonValue>> SourceRows;
				TArray<TSharedPtr<FJsonValue>> WarningRows;
				int32 ReadyCount = 0;
				int32 MissingCount = 0;
				int32 BlockedCount = 0;
				int32 TotalReferencers = 0;

				for (const FString& RawSourcePath : SourcePaths)
				{
					FString SourcePath = RawSourcePath;
					SourcePath.TrimStartAndEndInline();
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("requestedPath"), SourcePath);
					if (SourcePath.IsEmpty())
					{
						Row->SetStringField(TEXT("status"), TEXT("blocked_empty_source_path"));
						SourceRows.Add(MakeShared<FJsonValueObject>(Row));
						BlockedCount++;
						continue;
					}

					FAssetData SourceData = ResolveAssetDataByPath(AssetRegistry, SourcePath);
					if (!SourceData.IsValid())
					{
						Row->SetStringField(TEXT("status"), TEXT("missing"));
						Row->SetStringField(TEXT("reason"), TEXT("asset_not_found"));
						SourceRows.Add(MakeShared<FJsonValueObject>(Row));
						MissingCount++;
						continue;
					}

					UObject* SourceObject = SourceData.GetAsset();
					const FString SourceFamily = AssetFamilyFromObject(SourceObject, SourceData);
					const bool bSamePackage = SourceData.PackageName == TargetData.PackageName;
					const bool bSameClass = SourceData.AssetClassPath == TargetData.AssetClassPath;
					const bool bSameFamily = !SourceFamily.Equals(TEXT("unknown")) && SourceFamily == TargetFamily;

					TArray<FName> Referencers;
					AssetRegistry.GetReferencers(SourceData.PackageName, Referencers);
					TotalReferencers += Referencers.Num();

					Row->SetStringField(TEXT("status"), (!bSamePackage && (bSameClass || bSameFamily)) ? TEXT("ready") : TEXT("blocked"));
					Row->SetObjectField(TEXT("asset"), AssetDataToJsonDetailed(SourceData));
					Row->SetStringField(TEXT("family"), SourceFamily);
					Row->SetBoolField(TEXT("samePackageAsTarget"), bSamePackage);
					Row->SetBoolField(TEXT("sameClassAsTarget"), bSameClass);
					Row->SetBoolField(TEXT("sameFamilyAsTarget"), bSameFamily);
					Row->SetNumberField(TEXT("referencerCount"), Referencers.Num());
					Row->SetArrayField(TEXT("referencerPackages"), NameListToJsonArray(Referencers, MaxReferencers > 0 ? MaxReferencers : 1));
					Row->SetBoolField(TEXT("referencersTruncated"), Referencers.Num() > MaxReferencers);

					if (bSamePackage)
					{
						TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
						Warning->SetStringField(TEXT("source"), SourceData.PackageName.ToString());
						Warning->SetStringField(TEXT("code"), TEXT("source_is_target"));
						Warning->SetStringField(TEXT("message"), TEXT("A source asset is the same package as the target asset."));
						WarningRows.Add(MakeShared<FJsonValueObject>(Warning));
						BlockedCount++;
					}
					else if (!(bSameClass || bSameFamily))
					{
						TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
						Warning->SetStringField(TEXT("source"), SourceData.PackageName.ToString());
						Warning->SetStringField(TEXT("code"), TEXT("family_mismatch"));
						Warning->SetStringField(TEXT("message"), FString::Printf(TEXT("Source family '%s' does not match target family '%s'."), *SourceFamily, *TargetFamily));
						WarningRows.Add(MakeShared<FJsonValueObject>(Warning));
						BlockedCount++;
					}
					else
					{
						ReadyCount++;
					}

					SourceRows.Add(MakeShared<FJsonValueObject>(Row));
				}

				const bool bReady = ReadyCount == SourcePaths.Num() && MissingCount == 0 && BlockedCount == 0;
				OutStructured->SetStringField(TEXT("operation"), TEXT("asset_consolidate_plan"));
				OutStructured->SetStringField(TEXT("status"), bReady ? TEXT("ready") : TEXT("blocked"));
				OutStructured->SetBoolField(TEXT("ready"), bReady);
				OutStructured->SetBoolField(TEXT("readOnly"), true);
				OutStructured->SetBoolField(TEXT("execute"), false);
				OutStructured->SetStringField(TEXT("targetAsset"), TargetData.PackageName.ToString());
				OutStructured->SetObjectField(TEXT("target"), AssetDataToJsonDetailed(TargetData));
				OutStructured->SetStringField(TEXT("targetFamily"), TargetFamily);
				OutStructured->SetArrayField(TEXT("sources"), SourceRows);
				OutStructured->SetArrayField(TEXT("warnings"), WarningRows);
				OutStructured->SetNumberField(TEXT("sourceCount"), SourcePaths.Num());
				OutStructured->SetNumberField(TEXT("readyCount"), ReadyCount);
				OutStructured->SetNumberField(TEXT("missingCount"), MissingCount);
				OutStructured->SetNumberField(TEXT("blockedCount"), BlockedCount);
				OutStructured->SetNumberField(TEXT("totalReferencerCount"), TotalReferencers);
				OutStructured->SetArrayField(TEXT("recommendedNextTools"), StringArrayToJsonArray({
					TEXT("asset_referencers_list_v2"),
					TEXT("asset_dependency_graph_v2"),
					TEXT("asset_metadata_set_v2"),
					TEXT("asset_editor_save_safe"),
					TEXT("asset_redirectors_fixup_v2")
				}));
				OutSummary = FString::Printf(TEXT("Consolidate plan %s: %d ready, %d missing, %d blocked, %d total referencer packages."),
					bReady ? TEXT("ready") : TEXT("blocked"),
					ReadyCount,
					MissingCount,
					BlockedCount,
					TotalReferencers);
				return true;
			},
			nullptr,
			0
		});

		Registry.Register({
			TEXT("editor_subsystem_call_plan"),
			TEXT("Concrete P2 editor subsystem call planner. Reflects a subsystem/class function and classifies read/write risk without invoking it."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("subsystem_class"), FSololmcpSchemaBuilder::String(TEXT("Subsystem class path or short name, e.g. /Script/UnrealEd.EditorAssetSubsystem."))},
					{TEXT("class_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for subsystem_class."))},
					{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("Function name to inspect."))},
					{TEXT("arguments"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional proposed arguments. They are echoed for planning only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ClassPath;
				if (!Arguments->TryGetStringField(TEXT("subsystem_class"), ClassPath))
				{
					Arguments->TryGetStringField(TEXT("class_path"), ClassPath);
				}
				FString FunctionName;
				Arguments->TryGetStringField(TEXT("function_name"), FunctionName);
				ClassPath.TrimStartAndEndInline();
				FunctionName.TrimStartAndEndInline();
				if (ClassPath.IsEmpty() || FunctionName.IsEmpty())
				{
					OutError = TEXT("Missing subsystem_class/class_path or function_name.");
					return false;
				}

				if (!ClassPath.StartsWith(TEXT("/")))
				{
					if (ClassPath.Equals(TEXT("LevelEditorSubsystem"), ESearchCase::IgnoreCase))
					{
						ClassPath = TEXT("/Script/LevelEditor.LevelEditorSubsystem");
					}
					else
					{
						ClassPath = FString::Printf(TEXT("/Script/UnrealEd.%s"), *ClassPath);
					}
				}

				UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
				if (!Class)
				{
					Class = LoadObject<UClass>(nullptr, *ClassPath);
				}
				UFunction* Function = Class ? Class->FindFunctionByName(FName(*FunctionName)) : nullptr;

				TArray<TSharedPtr<FJsonValue>> ParameterRows;
				if (Function)
				{
					for (TFieldIterator<FProperty> PropertyIt(Function); PropertyIt; ++PropertyIt)
					{
						FProperty* Property = *PropertyIt;
						if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm))
						{
							continue;
						}
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("name"), Property->GetName());
						Row->SetStringField(TEXT("cppType"), Property->GetCPPType());
						Row->SetBoolField(TEXT("isReturn"), Property->HasAnyPropertyFlags(CPF_ReturnParm));
						Row->SetBoolField(TEXT("isOut"), Property->HasAnyPropertyFlags(CPF_OutParm));
						Row->SetBoolField(TEXT("isReference"), Property->HasAnyPropertyFlags(CPF_ReferenceParm));
						ParameterRows.Add(MakeShared<FJsonValueObject>(Row));
					}
				}

				const FString LowerName = FunctionName.ToLower();
				const bool bLooksReadOnlyByName =
					LowerName.StartsWith(TEXT("get")) ||
					LowerName.StartsWith(TEXT("is")) ||
					LowerName.StartsWith(TEXT("has")) ||
					LowerName.StartsWith(TEXT("find")) ||
					LowerName.StartsWith(TEXT("list")) ||
					LowerName.Contains(TEXT("query")) ||
					LowerName.Contains(TEXT("inspect"));
				const bool bLooksMutatingByName =
					LowerName.StartsWith(TEXT("set")) ||
					LowerName.StartsWith(TEXT("add")) ||
					LowerName.StartsWith(TEXT("remove")) ||
					LowerName.StartsWith(TEXT("delete")) ||
					LowerName.StartsWith(TEXT("destroy")) ||
					LowerName.StartsWith(TEXT("create")) ||
					LowerName.StartsWith(TEXT("spawn")) ||
					LowerName.StartsWith(TEXT("duplicate")) ||
					LowerName.StartsWith(TEXT("rename")) ||
					LowerName.StartsWith(TEXT("save")) ||
					LowerName.StartsWith(TEXT("open")) ||
					LowerName.StartsWith(TEXT("close")) ||
					LowerName.StartsWith(TEXT("run")) ||
					LowerName.StartsWith(TEXT("execute")) ||
					LowerName.Contains(TEXT("compile")) ||
					LowerName.Contains(TEXT("import"));
				const bool bPureOrConst = Function && Function->HasAnyFunctionFlags(FUNC_BlueprintPure | FUNC_Const);
				const bool bBlueprintCallable = Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
				const FString Risk = (!Function || bLooksMutatingByName)
					? TEXT("potential_editor_write_or_ui")
					: ((bPureOrConst || bLooksReadOnlyByName) ? TEXT("read_or_query") : TEXT("unknown_requires_harness"));

				TSharedRef<FJsonObject> Flags = MakeShared<FJsonObject>();
				Flags->SetBoolField(TEXT("blueprintCallable"), bBlueprintCallable);
				Flags->SetBoolField(TEXT("blueprintPure"), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
				Flags->SetBoolField(TEXT("const"), Function && Function->HasAnyFunctionFlags(FUNC_Const));
				Flags->SetBoolField(TEXT("static"), Function && Function->HasAnyFunctionFlags(FUNC_Static));
				Flags->SetBoolField(TEXT("exec"), Function && Function->HasAnyFunctionFlags(FUNC_Exec));

				OutStructured->SetStringField(TEXT("operation"), TEXT("editor_subsystem_call_plan"));
				OutStructured->SetStringField(TEXT("status"), Function ? TEXT("planned") : TEXT("blocked_function_missing"));
				OutStructured->SetBoolField(TEXT("readOnly"), true);
				OutStructured->SetBoolField(TEXT("execute"), false);
				OutStructured->SetStringField(TEXT("classPath"), ClassPath);
				OutStructured->SetBoolField(TEXT("classFound"), Class != nullptr);
				OutStructured->SetStringField(TEXT("functionName"), FunctionName);
				OutStructured->SetBoolField(TEXT("functionFound"), Function != nullptr);
				OutStructured->SetStringField(TEXT("riskClass"), Risk);
				OutStructured->SetStringField(TEXT("recommendedLane"), Risk == TEXT("read_or_query") ? TEXT("read") : TEXT("locked_write_or_ui"));
				OutStructured->SetObjectField(TEXT("functionFlags"), Flags);
				OutStructured->SetArrayField(TEXT("parameters"), ParameterRows);
				if (const TSharedPtr<FJsonObject>* ProposedArguments = nullptr; Arguments->TryGetObjectField(TEXT("arguments"), ProposedArguments) && ProposedArguments && ProposedArguments->IsValid())
				{
					OutStructured->SetObjectField(TEXT("proposedArguments"), *ProposedArguments);
				}
				OutSummary = Function
					? FString::Printf(TEXT("Planned reflected call %s.%s as %s."), *ClassPath, *FunctionName, *Risk)
					: FString::Printf(TEXT("Could not find reflected function %s.%s."), *ClassPath, *FunctionName);
				return Function != nullptr;
			},
			nullptr,
			0
		});

		Registry.Register({
			TEXT("editor_transaction_snapshot"),
			TEXT("Concrete P2 editor transaction snapshot. Read-only snapshot of the editor undo/redo buffer for queue checkpoints and rollback planning."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("max_transactions"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum recent transactions to include. Default 10."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				int32 MaxTransactions = 10;
				Arguments->TryGetNumberField(TEXT("max_transactions"), MaxTransactions);
				MaxTransactions = FMath::Clamp(MaxTransactions, 0, 50);

				UTransBuffer* TransBuffer = (GEditor && GEditor->Trans) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
				OutStructured->SetStringField(TEXT("operation"), TEXT("editor_transaction_snapshot"));
				OutStructured->SetBoolField(TEXT("readOnly"), true);
				OutStructured->SetBoolField(TEXT("available"), TransBuffer != nullptr);
				if (!TransBuffer)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("unavailable"));
					if (GEditor && GEditor->Trans)
					{
						OutStructured->SetStringField(TEXT("transactorClass"), GEditor->Trans->GetClass()->GetPathName());
					}
					OutSummary = TEXT("Editor transaction buffer is unavailable.");
					return true;
				}

				const int32 QueueLength = TransBuffer->GetQueueLength();
				const int32 StartIndex = MaxTransactions > 0 ? FMath::Max(0, QueueLength - MaxTransactions) : QueueLength;
				TArray<TSharedPtr<FJsonValue>> TransactionRows;
				for (int32 Index = StartIndex; Index < QueueLength; ++Index)
				{
					const FTransaction* Transaction = TransBuffer->GetTransaction(Index);
					if (!Transaction)
					{
						continue;
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetNumberField(TEXT("index"), Index);
					Row->SetStringField(TEXT("id"), Transaction->GetId().ToString());
					Row->SetStringField(TEXT("title"), Transaction->GetTitle().ToString());
					Row->SetNumberField(TEXT("dataSizeBytes"), static_cast<double>(Transaction->DataSize()));
					TransactionRows.Add(MakeShared<FJsonValueObject>(Row));
				}

				FText UndoText;
				FText RedoText;
				const bool bCanUndo = TransBuffer->CanUndo(&UndoText);
				const bool bCanRedo = TransBuffer->CanRedo(&RedoText);
				OutStructured->SetStringField(TEXT("status"), TEXT("ok"));
				OutStructured->SetNumberField(TEXT("queueLength"), QueueLength);
				OutStructured->SetNumberField(TEXT("undoCount"), TransBuffer->GetUndoCount());
				OutStructured->SetNumberField(TEXT("undoSizeBytes"), static_cast<double>(TransBuffer->GetUndoSize()));
				OutStructured->SetNumberField(TEXT("currentUndoBarrier"), TransBuffer->GetCurrentUndoBarrier());
				OutStructured->SetBoolField(TEXT("active"), TransBuffer->IsActive());
				OutStructured->SetBoolField(TEXT("canUndo"), bCanUndo);
				OutStructured->SetBoolField(TEXT("canRedo"), bCanRedo);
				OutStructured->SetStringField(TEXT("undoTitle"), UndoText.ToString());
				OutStructured->SetStringField(TEXT("redoTitle"), RedoText.ToString());
				OutStructured->SetStringField(TEXT("resetReason"), TransBuffer->ResetReason.ToString());
				OutStructured->SetArrayField(TEXT("recentTransactions"), TransactionRows);
				OutSummary = FString::Printf(TEXT("Transaction snapshot: queue=%d, undoCount=%d, active=%s."),
					QueueLength,
					TransBuffer->GetUndoCount(),
					TransBuffer->IsActive() ? TEXT("true") : TEXT("false"));
				return true;
			},
			nullptr,
			0
		});

		Registry.Register({
			TEXT("editor_asset_scripting_receipt_validate"),
			TEXT("Concrete P2 asset scripting receipt validator. Validates target binding, Asset Registry readback, dirty-state, receipt status, and optional object metadata."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Target asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional receipt object from a previous asset scripting call."))},
					{TEXT("expected_status"), FSololmcpSchemaBuilder::String(TEXT("Optional expected receipt status, e.g. completed."))},
					{TEXT("expected_metadata"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional expected UPackage.FMetaData key/value pairs."))},
					{TEXT("require_saved"), FSololmcpSchemaBuilder::Boolean(TEXT("Require package to be clean. Default false."))},
					{TEXT("allow_dirty"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow dirty package. Default false."))},
					{TEXT("require_readback"), FSololmcpSchemaBuilder::Boolean(TEXT("Require Asset Registry readback. Default true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TSharedPtr<FJsonObject> Receipt;
				if (const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr; Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) && ReceiptPtr && ReceiptPtr->IsValid())
				{
					Receipt = *ReceiptPtr;
				}

				FString AssetPath;
				FString PathError;
				if (!TryGetAssetPathArgument(Arguments, AssetPath, PathError) && Receipt.IsValid())
				{
					if (!Receipt->TryGetStringField(TEXT("assetPath"), AssetPath) &&
						!Receipt->TryGetStringField(TEXT("targetAsset"), AssetPath) &&
						!Receipt->TryGetStringField(TEXT("target_asset"), AssetPath) &&
						!Receipt->TryGetStringField(TEXT("packageName"), AssetPath) &&
						!Receipt->TryGetStringField(TEXT("objectPath"), AssetPath))
					{
						const TArray<TSharedPtr<FJsonValue>>* ImportedPaths = nullptr;
						if (Receipt->TryGetArrayField(TEXT("importedObjectPaths"), ImportedPaths) && ImportedPaths && ImportedPaths->Num() > 0 && (*ImportedPaths)[0].IsValid())
						{
							AssetPath = (*ImportedPaths)[0]->AsString();
						}
					}
				}
				AssetPath.TrimStartAndEndInline();

				bool bRequireSaved = false;
				bool bAllowDirty = false;
				bool bRequireReadback = true;
				Arguments->TryGetBoolField(TEXT("require_saved"), bRequireSaved);
				Arguments->TryGetBoolField(TEXT("allow_dirty"), bAllowDirty);
				Arguments->TryGetBoolField(TEXT("require_readback"), bRequireReadback);

				FString ExpectedStatus;
				Arguments->TryGetStringField(TEXT("expected_status"), ExpectedStatus);
				ExpectedStatus.TrimStartAndEndInline();

				TArray<TSharedPtr<FJsonValue>> CheckRows;
				auto AddCheck = [&CheckRows](const FString& Name, bool bPass, const FString& Detail)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Name);
					Row->SetBoolField(TEXT("pass"), bPass);
					Row->SetStringField(TEXT("detail"), Detail);
					CheckRows.Add(MakeShared<FJsonValueObject>(Row));
				};

				bool bValid = true;
				const bool bHasTargetBinding = !AssetPath.IsEmpty();
				AddCheck(TEXT("target_binding"), bHasTargetBinding, bHasTargetBinding ? AssetPath : TEXT("missing asset_path/target_asset or receipt target binding"));
				bValid &= bHasTargetBinding;

				FAssetData AssetData;
				UObject* LoadedAsset = nullptr;
				UPackage* Package = nullptr;
				if (bHasTargetBinding)
				{
					FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
					IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
					AssetData = ResolveAssetDataByPath(AssetRegistry, AssetPath);
					const bool bReadback = AssetData.IsValid();
					AddCheck(TEXT("asset_registry_readback"), !bRequireReadback || bReadback, bReadback ? AssetData.GetObjectPathString() : TEXT("asset not found"));
					bValid &= (!bRequireReadback || bReadback);
					if (bReadback)
					{
						LoadedAsset = AssetData.GetAsset();
						Package = LoadedAsset ? LoadedAsset->GetOutermost() : FindPackage(nullptr, *AssetData.PackageName.ToString());
					}
				}

				if (Receipt.IsValid() && !ExpectedStatus.IsEmpty())
				{
					FString ActualStatus;
					Receipt->TryGetStringField(TEXT("status"), ActualStatus);
					const bool bStatusOk = ActualStatus.Equals(ExpectedStatus, ESearchCase::IgnoreCase);
					AddCheck(TEXT("receipt_status"), bStatusOk, FString::Printf(TEXT("expected='%s' actual='%s'"), *ExpectedStatus, *ActualStatus));
					bValid &= bStatusOk;
				}
				else if (!ExpectedStatus.IsEmpty())
				{
					AddCheck(TEXT("receipt_status"), false, TEXT("expected_status was supplied but receipt object is missing"));
					bValid = false;
				}

				if (Package)
				{
					const bool bDirty = Package->IsDirty();
					const bool bDirtyOk = bAllowDirty || !bDirty;
					AddCheck(TEXT("dirty_state"), bDirtyOk, bDirty ? TEXT("package is dirty") : TEXT("package is clean"));
					bValid &= bDirtyOk;
					if (bRequireSaved)
					{
						AddCheck(TEXT("require_saved"), !bDirty, bDirty ? TEXT("package still dirty") : TEXT("package clean after save/readback"));
						bValid &= !bDirty;
					}
				}
				else if (bHasTargetBinding)
				{
					AddCheck(TEXT("dirty_state"), !bRequireSaved, TEXT("package not loaded; dirty state unavailable"));
					bValid &= !bRequireSaved;
				}

				TSharedRef<FJsonObject> ExpectedMetadataNormalized = MakeShared<FJsonObject>();
				if (const TSharedPtr<FJsonObject>* ExpectedMetadataPtr = nullptr; Arguments->TryGetObjectField(TEXT("expected_metadata"), ExpectedMetadataPtr) && ExpectedMetadataPtr && ExpectedMetadataPtr->IsValid())
				{
					TSharedRef<FJsonObject> ActualMetadata = ObjectMetadataToJson(LoadedAsset);
					TArray<TSharedPtr<FJsonValue>> MetadataRows;
					for (const auto& Pair : (*ExpectedMetadataPtr)->Values)
					{
						FString Key(*Pair.Key);
						Key.TrimStartAndEndInline();
						FString ExpectedValue;
						FString MetadataError;
						const bool bValueParsed = JsonValueToMetadataString(Pair.Value, ExpectedValue, MetadataError);
						FString ActualValue;
						const bool bHasActualValue = bValueParsed && ActualMetadata->TryGetStringField(Key, ActualValue);
						const bool bMatch = bValueParsed && bHasActualValue && ActualValue == ExpectedValue;

						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("key"), Key);
						Row->SetBoolField(TEXT("pass"), bMatch);
						if (bValueParsed)
						{
							Row->SetStringField(TEXT("expected"), ExpectedValue);
							ExpectedMetadataNormalized->SetStringField(Key, ExpectedValue);
						}
						else
						{
							Row->SetStringField(TEXT("error"), MetadataError);
						}
						Row->SetBoolField(TEXT("actualPresent"), bHasActualValue);
						if (bHasActualValue)
						{
							Row->SetStringField(TEXT("actual"), ActualValue);
						}
						MetadataRows.Add(MakeShared<FJsonValueObject>(Row));
						bValid &= bMatch;
					}
					AddCheck(TEXT("expected_metadata"), bValid, FString::Printf(TEXT("%d expected metadata entrie(s) checked."), (*ExpectedMetadataPtr)->Values.Num()));
					OutStructured->SetArrayField(TEXT("metadataChecks"), MetadataRows);
					OutStructured->SetObjectField(TEXT("expectedMetadata"), ExpectedMetadataNormalized);
					OutStructured->SetObjectField(TEXT("actualMetadata"), ActualMetadata);
				}

				OutStructured->SetStringField(TEXT("operation"), TEXT("editor_asset_scripting_receipt_validate"));
				OutStructured->SetStringField(TEXT("status"), bValid ? TEXT("valid") : TEXT("invalid"));
				OutStructured->SetBoolField(TEXT("valid"), bValid);
				OutStructured->SetBoolField(TEXT("requireSaved"), bRequireSaved);
				OutStructured->SetBoolField(TEXT("allowDirty"), bAllowDirty);
				OutStructured->SetBoolField(TEXT("requireReadback"), bRequireReadback);
				OutStructured->SetStringField(TEXT("targetAsset"), AssetPath);
				OutStructured->SetArrayField(TEXT("checks"), CheckRows);
				if (AssetData.IsValid())
				{
					OutStructured->SetObjectField(TEXT("readback"), AssetDataToJsonDetailed(AssetData));
				}
				OutSummary = FString::Printf(TEXT("Asset scripting receipt validation %s for '%s'."),
					bValid ? TEXT("passed") : TEXT("failed"),
					*AssetPath);
				return bValid;
			},
			nullptr,
			0
		});

		Registry.Register({
			TEXT("actor_group_create"),
			TEXT("Concrete P2 actor group create. Defaults to dry-run; set execute=true to group level actors by exact name, label, path, or current selection."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Actor names, labels, or paths.")), TEXT("Actors to group."))},
					{TEXT("actor_ids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Alias for actors.")), TEXT("Alias for actors."))},
					{TEXT("use_selection"), FSololmcpSchemaBuilder::Boolean(TEXT("Use current editor selection when actors are omitted. Default false."))},
					{TEXT("force_grouping_active"), FSololmcpSchemaBuilder::Boolean(TEXT("Temporarily enable grouping if disabled. Default true."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}
				TArray<AActor*> Actors;
				TArray<TSharedPtr<FJsonValue>> ActorRows;
				TArray<TSharedPtr<FJsonValue>> ProblemRows;
				if (!ResolveActorListForGroupTool(Arguments, World, true, Actors, ActorRows, ProblemRows, OutError))
				{
					return false;
				}
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bForceGroupingActive = true;
				Arguments->TryGetBoolField(TEXT("force_grouping_active"), bForceGroupingActive);
				UActorGroupingUtils* GroupingUtils = UActorGroupingUtils::Get();
				const bool bGroupingWasActive = UActorGroupingUtils::IsGroupingActive();
				const bool bEnoughActors = Actors.Num() >= 2;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				const bool bCanGroup = GroupingUtils && GroupingUtils->CanGroupActors(Actors);
#else
				// UActorGroupingUtils::CanGroupActors is 5.4+. Before that the editor applies the
				// same minimum it enforces internally: grouping needs more than one actor.
				const bool bCanGroup = GroupingUtils && Actors.Num() > 1;
#endif
				bool bSameLevel = true;
				ULevel* ActorLevel = Actors.Num() > 0 ? Actors[0]->GetLevel() : nullptr;
				for (AActor* Actor : Actors)
				{
					if (!Actor || Actor->IsA<AGroupActor>() || Actor->GetLevel() != ActorLevel)
					{
						bSameLevel = false;
						break;
					}
				}
				const bool bReady = ProblemRows.Num() == 0 && bEnoughActors && bCanGroup && bSameLevel && (bGroupingWasActive || bForceGroupingActive);

				OutStructured->SetStringField(TEXT("operation"), TEXT("actor_group_create"));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("groupingWasActive"), bGroupingWasActive);
				OutStructured->SetBoolField(TEXT("forceGroupingActive"), bForceGroupingActive);
				OutStructured->SetBoolField(TEXT("ready"), bReady);
				OutStructured->SetArrayField(TEXT("actors"), ActorRows);
				OutStructured->SetArrayField(TEXT("problems"), ProblemRows);
				OutStructured->SetNumberField(TEXT("actorCount"), Actors.Num());
				OutStructured->SetBoolField(TEXT("canGroup"), bCanGroup);
				OutStructured->SetBoolField(TEXT("sameLevel"), bSameLevel);

				if (!bReady)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked"));
					OutSummary = FString::Printf(TEXT("Actor group create blocked: actors=%d, problems=%d, canGroup=%s, sameLevel=%s."),
						Actors.Num(),
						ProblemRows.Num(),
						bCanGroup ? TEXT("true") : TEXT("false"),
						bSameLevel ? TEXT("true") : TEXT("false"));
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run actor group create: %d actor(s)."), Actors.Num());
					return true;
				}

				if (!bGroupingWasActive && bForceGroupingActive)
				{
					UActorGroupingUtils::SetGroupingActive(true);
				}
				AGroupActor* GroupActor = GroupingUtils->GroupActors(Actors);
				if (!bGroupingWasActive && bForceGroupingActive)
				{
					UActorGroupingUtils::SetGroupingActive(false);
				}
				if (!GroupActor)
				{
					OutError = TEXT("UActorGroupingUtils::GroupActors returned null.");
					return false;
				}

				OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
				OutStructured->SetObjectField(TEXT("group"), GroupActorToJsonLocal(GroupActor));
				OutStructured->SetNumberField(TEXT("activeGroupCount"), AGroupActor::NumActiveGroups(false, true));
				OutSummary = FString::Printf(TEXT("Created actor group '%s' with %d actor(s)."), *GroupActor->GetPathName(), Actors.Num());
				return true;
			}
		});

		Registry.Register({
			TEXT("actor_group_add"),
			TEXT("Concrete P2 actor group add. Defaults to dry-run; set execute=true to add actors to an existing AGroupActor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("group_actor"), FSololmcpSchemaBuilder::String(TEXT("Target AGroupActor name, label, or path."))},
					{TEXT("group_actor_id"), FSololmcpSchemaBuilder::String(TEXT("Alias for group_actor."))},
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Actor names, labels, or paths.")), TEXT("Actors to add."))},
					{TEXT("actor_ids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Alias for actors.")), TEXT("Alias for actors."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}
				AGroupActor* GroupActor = nullptr;
				if (!ResolveGroupActorArgument(Arguments, World, GroupActor, OutError))
				{
					return false;
				}
				TArray<AActor*> Actors;
				TArray<TSharedPtr<FJsonValue>> ActorRows;
				TArray<TSharedPtr<FJsonValue>> ProblemRows;
				if (!ResolveActorListForGroupTool(Arguments, World, false, Actors, ActorRows, ProblemRows, OutError))
				{
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> WarningRows;
				for (AActor* Actor : Actors)
				{
					if (!Actor || Actor == GroupActor || Actor->IsA<AGroupActor>() || Actor->GetLevel() != GroupActor->GetLevel())
					{
						TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
						Warning->SetStringField(TEXT("actor"), Actor ? Actor->GetPathName() : TEXT("(null)"));
						Warning->SetStringField(TEXT("code"), TEXT("invalid_group_member"));
						WarningRows.Add(MakeShared<FJsonValueObject>(Warning));
					}
				}
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				const bool bReady = Actors.Num() > 0 && ProblemRows.Num() == 0 && WarningRows.Num() == 0;
				OutStructured->SetStringField(TEXT("operation"), TEXT("actor_group_add"));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetObjectField(TEXT("groupBefore"), GroupActorToJsonLocal(GroupActor));
				OutStructured->SetArrayField(TEXT("actors"), ActorRows);
				OutStructured->SetArrayField(TEXT("problems"), ProblemRows);
				OutStructured->SetArrayField(TEXT("warnings"), WarningRows);
				OutStructured->SetBoolField(TEXT("ready"), bReady);

				if (!bReady)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked"));
					OutSummary = FString::Printf(TEXT("Actor group add blocked: actors=%d, problems=%d, warnings=%d."),
						Actors.Num(),
						ProblemRows.Num(),
						WarningRows.Num());
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run actor group add: %d actor(s) -> %s."), Actors.Num(), *GroupActor->GetPathName());
					return true;
				}

				{
					const FScopedTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Add Actors To Group")));
					for (AActor* Actor : Actors)
					{
						GroupActor->Add(*Actor);
					}
					GroupActor->CenterGroupLocation();
					GroupActor->MarkPackageDirty();
				}
				OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
				OutStructured->SetObjectField(TEXT("groupAfter"), GroupActorToJsonLocal(GroupActor));
				OutSummary = FString::Printf(TEXT("Added %d actor(s) to group '%s'."), Actors.Num(), *GroupActor->GetPathName());
				return true;
			}
		});

		Registry.Register({
			TEXT("actor_group_remove"),
			TEXT("Concrete P2 actor group remove. Defaults to dry-run; set execute=true to remove actors from their parent group or a specified AGroupActor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("group_actor"), FSololmcpSchemaBuilder::String(TEXT("Optional target AGroupActor name, label, or path."))},
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Actor names, labels, or paths.")), TEXT("Actors to remove."))},
					{TEXT("actor_ids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Alias for actors.")), TEXT("Alias for actors."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}
				AGroupActor* ExplicitGroup = nullptr;
				FString GroupIdentifier;
				const bool bHasExplicitGroup =
					Arguments->TryGetStringField(TEXT("group_actor"), GroupIdentifier) ||
					Arguments->TryGetStringField(TEXT("group_actor_id"), GroupIdentifier) ||
					Arguments->TryGetStringField(TEXT("group_name"), GroupIdentifier);
				if (bHasExplicitGroup && !ResolveGroupActorArgument(Arguments, World, ExplicitGroup, OutError))
				{
					return false;
				}
				TArray<AActor*> Actors;
				TArray<TSharedPtr<FJsonValue>> ActorRows;
				TArray<TSharedPtr<FJsonValue>> ProblemRows;
				if (!ResolveActorListForGroupTool(Arguments, World, false, Actors, ActorRows, ProblemRows, OutError))
				{
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> PlanRows;
				int32 RemovableCount = 0;
				for (AActor* Actor : Actors)
				{
					AGroupActor* ParentGroup = ExplicitGroup ? ExplicitGroup : (Actor ? Cast<AGroupActor>(Actor->GroupActor.Get()) : nullptr);
					const bool bContained = ParentGroup && Actor && ParentGroup->Contains(*Actor);
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetObjectField(TEXT("actor"), ActorToJsonLocal(Actor));
					Row->SetBoolField(TEXT("hasGroup"), ParentGroup != nullptr);
					Row->SetBoolField(TEXT("containedInTargetGroup"), bContained);
					if (ParentGroup)
					{
						Row->SetObjectField(TEXT("group"), GroupActorToJsonLocal(ParentGroup, false));
					}
					Row->SetStringField(TEXT("status"), bContained ? TEXT("ready") : TEXT("blocked_not_in_group"));
					if (bContained)
					{
						RemovableCount++;
					}
					PlanRows.Add(MakeShared<FJsonValueObject>(Row));
				}

				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				const bool bReady = Actors.Num() > 0 && ProblemRows.Num() == 0 && RemovableCount == Actors.Num();
				OutStructured->SetStringField(TEXT("operation"), TEXT("actor_group_remove"));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("ready"), bReady);
				OutStructured->SetArrayField(TEXT("actors"), ActorRows);
				OutStructured->SetArrayField(TEXT("problems"), ProblemRows);
				OutStructured->SetArrayField(TEXT("plan"), PlanRows);
				OutStructured->SetNumberField(TEXT("removableCount"), RemovableCount);

				if (!bReady)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked"));
					OutSummary = FString::Printf(TEXT("Actor group remove blocked: actors=%d, removable=%d, problems=%d."),
						Actors.Num(),
						RemovableCount,
						ProblemRows.Num());
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run actor group remove: %d actor(s)."), Actors.Num());
					return true;
				}

				{
					const FScopedTransaction Transaction(FText::FromString(TEXT("SOMOLMCP Remove Actors From Group")));
					for (AActor* Actor : Actors)
					{
						AGroupActor* ParentGroup = ExplicitGroup ? ExplicitGroup : (Actor ? Cast<AGroupActor>(Actor->GroupActor.Get()) : nullptr);
						if (ParentGroup && Actor && ParentGroup->Contains(*Actor))
						{
							ParentGroup->Remove(*Actor);
							if (IsValid(ParentGroup))
							{
								ParentGroup->CenterGroupLocation();
								ParentGroup->MarkPackageDirty();
							}
						}
					}
				}
				OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
				OutStructured->SetNumberField(TEXT("activeGroupCount"), AGroupActor::NumActiveGroups(false, true));
				OutSummary = FString::Printf(TEXT("Removed %d actor(s) from actor group(s)."), Actors.Num());
				return true;
			}
		});

		Registry.Register({
			TEXT("actor_group_ungroup"),
			TEXT("Concrete P2 actor group ungroup. Defaults to dry-run; set execute=true to disband a group or groups containing supplied actors."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("group_actor"), FSololmcpSchemaBuilder::String(TEXT("Optional target AGroupActor name, label, or path."))},
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Actor names, labels, or paths.")), TEXT("Actors whose groups should be disbanded."))},
					{TEXT("actor_ids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Alias for actors.")), TEXT("Alias for actors."))},
					{TEXT("use_selection"), FSololmcpSchemaBuilder::Boolean(TEXT("Use current editor selection when no group_actor/actors are supplied. Default false."))},
					{TEXT("force_grouping_active"), FSololmcpSchemaBuilder::Boolean(TEXT("Temporarily enable grouping if disabled. Default true."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}
				AGroupActor* ExplicitGroup = nullptr;
				FString GroupIdentifier;
				const bool bHasExplicitGroup =
					Arguments->TryGetStringField(TEXT("group_actor"), GroupIdentifier) ||
					Arguments->TryGetStringField(TEXT("group_actor_id"), GroupIdentifier) ||
					Arguments->TryGetStringField(TEXT("group_name"), GroupIdentifier);
				if (bHasExplicitGroup && !ResolveGroupActorArgument(Arguments, World, ExplicitGroup, OutError))
				{
					return false;
				}
				TArray<AActor*> Actors;
				TArray<TSharedPtr<FJsonValue>> ActorRows;
				TArray<TSharedPtr<FJsonValue>> ProblemRows;
				if (!ExplicitGroup)
				{
					if (!ResolveActorListForGroupTool(Arguments, World, true, Actors, ActorRows, ProblemRows, OutError))
					{
						return false;
					}
				}

				TArray<AGroupActor*> GroupsToDisband;
				if (ExplicitGroup)
				{
					GroupsToDisband.Add(ExplicitGroup);
				}
				else
				{
					for (AActor* Actor : Actors)
					{
						if (AGroupActor* Root = AGroupActor::GetRootForActor(Actor, true))
						{
							GroupsToDisband.AddUnique(Root);
						}
						else if (AGroupActor* Parent = (Actor ? Cast<AGroupActor>(Actor->GroupActor.Get()) : nullptr))
						{
							GroupsToDisband.AddUnique(Parent);
						}
					}
				}

				TArray<TSharedPtr<FJsonValue>> GroupRows;
				for (AGroupActor* GroupActor : GroupsToDisband)
				{
					GroupRows.Add(MakeShared<FJsonValueObject>(GroupActorToJsonLocal(GroupActor)));
				}
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bForceGroupingActive = true;
				Arguments->TryGetBoolField(TEXT("force_grouping_active"), bForceGroupingActive);
				const bool bGroupingWasActive = UActorGroupingUtils::IsGroupingActive();
				const bool bReady = ProblemRows.Num() == 0 && GroupsToDisband.Num() > 0 && (bGroupingWasActive || bForceGroupingActive);
				OutStructured->SetStringField(TEXT("operation"), TEXT("actor_group_ungroup"));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("ready"), bReady);
				OutStructured->SetBoolField(TEXT("groupingWasActive"), bGroupingWasActive);
				OutStructured->SetBoolField(TEXT("forceGroupingActive"), bForceGroupingActive);
				OutStructured->SetArrayField(TEXT("actors"), ActorRows);
				OutStructured->SetArrayField(TEXT("problems"), ProblemRows);
				OutStructured->SetArrayField(TEXT("groups"), GroupRows);
				OutStructured->SetNumberField(TEXT("groupCount"), GroupsToDisband.Num());
				OutStructured->SetNumberField(TEXT("activeGroupCountBefore"), AGroupActor::NumActiveGroups(false, true));

				if (!bReady)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked"));
					OutSummary = FString::Printf(TEXT("Actor group ungroup blocked: groups=%d, problems=%d."),
						GroupsToDisband.Num(),
						ProblemRows.Num());
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run actor group ungroup: %d group(s)."), GroupsToDisband.Num());
					return true;
				}

				if (!bGroupingWasActive && bForceGroupingActive)
				{
					UActorGroupingUtils::SetGroupingActive(true);
				}
				UActorGroupingUtils* GroupingUtils = UActorGroupingUtils::Get();
				TArray<AActor*> ActorsToUngroup;
				if (ExplicitGroup)
				{
					ActorsToUngroup.Add(ExplicitGroup);
				}
				else
				{
					ActorsToUngroup = Actors;
				}
				GroupingUtils->UngroupActors(ActorsToUngroup);
				if (!bGroupingWasActive && bForceGroupingActive)
				{
					UActorGroupingUtils::SetGroupingActive(false);
				}
				OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
				OutStructured->SetNumberField(TEXT("activeGroupCountAfter"), AGroupActor::NumActiveGroups(false, true));
				OutSummary = FString::Printf(TEXT("Ungrouped %d actor group(s)."), GroupsToDisband.Num());
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_editor_compile_active"),
			TEXT("Concrete P2 asset editor compile active. Defaults to dry-run; set execute=true to compile a target Blueprint or currently open Blueprint assets."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional Blueprint asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("include_open_editors"), FSololmcpSchemaBuilder::Boolean(TEXT("When asset_path is omitted, scan open asset editors. Default true."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run compile plan."))},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save successfully compiled Blueprint assets. Default false."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bSaveAsset = false;
				bool bIncludeOpenEditors = true;
				Arguments->TryGetBoolField(TEXT("save_asset"), bSaveAsset);
				Arguments->TryGetBoolField(TEXT("include_open_editors"), bIncludeOpenEditors);

				TArray<UObject*> CandidateAssets;
				auto AddCandidate = [&CandidateAssets](UObject* Asset)
				{
					if (Asset && !CandidateAssets.Contains(Asset))
					{
						CandidateAssets.Add(Asset);
					}
				};

				FString AssetPath;
				FString AssetPathError;
				if (TryGetAssetPathArgument(Arguments, AssetPath, AssetPathError))
				{
					FAssetData AssetData;
					FString ResolvedPath;
					UObject* Asset = LoadAssetFlexible(AssetPath, AssetData, ResolvedPath, OutError);
					if (!Asset)
					{
						return false;
					}
					AddCandidate(Asset);
				}
				else if (bIncludeOpenEditors)
				{
					if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
					{
						for (UObject* Asset : AssetEditorSubsystem->GetAllEditedAssets())
						{
							AddCandidate(Asset);
						}
					}
				}

				TArray<TSharedPtr<FJsonValue>> Rows;
				int32 BlueprintCount = 0;
				int32 CompiledCount = 0;
				int32 FailedCount = 0;
				int32 SavedCount = 0;
				for (UObject* Asset : CandidateAssets)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetObjectField(TEXT("asset"), FSololmcpEditorServices::MakeObjectReference(Asset));
					UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
					Row->SetBoolField(TEXT("isBlueprint"), Blueprint != nullptr);
					if (!Blueprint)
					{
						Row->SetStringField(TEXT("status"), TEXT("skipped_not_blueprint"));
						Rows.Add(MakeShared<FJsonValueObject>(Row));
						continue;
					}

					BlueprintCount++;
					const FString StatusBefore = StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
					Row->SetStringField(TEXT("statusBefore"), StatusBefore);
					Row->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());

					if (!bExecute)
					{
						Row->SetStringField(TEXT("status"), TEXT("dry_run"));
						Rows.Add(MakeShared<FJsonValueObject>(Row));
						continue;
					}

					FKismetEditorUtilities::CompileBlueprint(Blueprint);
					const FString StatusAfter = StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
					const bool bCompileOk = Blueprint->Status != BS_Error;
					Row->SetStringField(TEXT("status"), bCompileOk ? TEXT("compiled") : TEXT("compile_failed"));
					Row->SetStringField(TEXT("statusAfter"), StatusAfter);
					Row->SetBoolField(TEXT("compileOk"), bCompileOk);
					if (bCompileOk)
					{
						CompiledCount++;
						if (bSaveAsset)
						{
							FString SaveError;
							if (Context.Services.SaveAsset(Blueprint->GetPathName(), false, SaveError))
							{
								Row->SetBoolField(TEXT("saved"), true);
								SavedCount++;
							}
							else
							{
								Row->SetBoolField(TEXT("saved"), false);
								Row->SetStringField(TEXT("saveError"), SaveError);
							}
						}
					}
					else
					{
						FailedCount++;
					}
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}

				const bool bHasBlueprint = BlueprintCount > 0;
				const bool bValid = !bExecute || (bHasBlueprint && FailedCount == 0);
				OutStructured->SetStringField(TEXT("operation"), TEXT("asset_editor_compile_active"));
				OutStructured->SetStringField(TEXT("status"), !bHasBlueprint ? TEXT("no_compilable_blueprints") : (bExecute ? (FailedCount == 0 ? TEXT("completed") : TEXT("failed")) : TEXT("dry_run")));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("saveAsset"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("includeOpenEditors"), bIncludeOpenEditors);
				OutStructured->SetArrayField(TEXT("assets"), Rows);
				OutStructured->SetNumberField(TEXT("candidateCount"), CandidateAssets.Num());
				OutStructured->SetNumberField(TEXT("blueprintCount"), BlueprintCount);
				OutStructured->SetNumberField(TEXT("compiledCount"), CompiledCount);
				OutStructured->SetNumberField(TEXT("failedCount"), FailedCount);
				OutStructured->SetNumberField(TEXT("savedCount"), SavedCount);
				if (!bExecute && bHasBlueprint)
				{
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
				}
				OutSummary = FString::Printf(TEXT("Blueprint compile active %s: candidates=%d, blueprints=%d, compiled=%d, failed=%d."),
					bExecute ? TEXT("execute") : TEXT("dry-run"),
					CandidateAssets.Num(),
					BlueprintCount,
					CompiledCount,
					FailedCount);
				return bValid;
			}
		});

		Registry.Register({
			TEXT("editor_utility_blueprint_run_safe"),
			TEXT("Concrete P2 Editor Utility Blueprint safe-run. Dry-run by default; execute requires both execute=true and allow_user_script_execution=true."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Editor Utility Blueprint asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, only checks CanRun."))},
					{TEXT("allow_user_script_execution"), FSololmcpSchemaBuilder::Boolean(TEXT("Required true in addition to execute=true to run user editor utility code."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}
				UObject* Asset = AssetData.GetAsset();
				if (!Asset)
				{
					OutError = FString::Printf(TEXT("Failed to load editor utility asset: '%s'."), *AssetData.GetObjectPathString());
					return false;
				}
				UEditorUtilitySubsystem* UtilitySubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr;
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bAllowUserScriptExecution = false;
				Arguments->TryGetBoolField(TEXT("allow_user_script_execution"), bAllowUserScriptExecution);
				const bool bCanRun = UtilitySubsystem && UtilitySubsystem->CanRun(Asset);
				const bool bLooksLikeEditorUtility = Asset->IsA<UEditorUtilityBlueprint>() || Asset->GetClass()->GetName().Contains(TEXT("EditorUtility"));

				OutStructured->SetStringField(TEXT("operation"), TEXT("editor_utility_blueprint_run_safe"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetObjectField(TEXT("asset"), AssetDataToJsonDetailed(AssetData));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("allowUserScriptExecution"), bAllowUserScriptExecution);
				OutStructured->SetBoolField(TEXT("subsystemAvailable"), UtilitySubsystem != nullptr);
				OutStructured->SetBoolField(TEXT("canRun"), bCanRun);
				OutStructured->SetBoolField(TEXT("looksLikeEditorUtility"), bLooksLikeEditorUtility);

				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), bCanRun ? TEXT("dry_run") : TEXT("blocked_cannot_run"));
					OutStructured->SetBoolField(TEXT("requires_execute"), bCanRun);
					OutSummary = FString::Printf(TEXT("Dry-run Editor Utility Blueprint safe-run for '%s': canRun=%s."),
						*AssetData.GetObjectPathString(),
						bCanRun ? TEXT("true") : TEXT("false"));
					return true;
				}
				if (!bAllowUserScriptExecution)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked_requires_user_script_execution"));
					OutError = TEXT("editor_utility_blueprint_run_safe requires allow_user_script_execution=true when execute=true.");
					return false;
				}
				if (!UtilitySubsystem || !bCanRun)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked_cannot_run"));
					OutError = TEXT("Editor Utility subsystem unavailable or asset cannot run.");
					return false;
				}

				const bool bRan = UtilitySubsystem->TryRun(Asset);
				OutStructured->SetStringField(TEXT("status"), bRan ? TEXT("completed") : TEXT("failed"));
				OutStructured->SetBoolField(TEXT("ran"), bRan);
				OutSummary = FString::Printf(TEXT("Editor Utility Blueprint run %s for '%s'."), bRan ? TEXT("completed") : TEXT("failed"), *AssetData.GetObjectPathString());
				return bRan;
			}
		});

		Registry.Register({
			TEXT("editor_utility_widget_run_safe"),
			TEXT("Concrete P2 Editor Utility Widget safe-run. Dry-run by default; execute requires both execute=true and allow_user_script_execution=true."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Editor Utility Widget Blueprint asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("spawn_tab"), FSololmcpSchemaBuilder::Boolean(TEXT("Spawn/register the widget tab when executing. Default true."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, only checks type/subsystem readiness."))},
					{TEXT("allow_user_script_execution"), FSololmcpSchemaBuilder::Boolean(TEXT("Required true in addition to execute=true to run/spawn user editor utility UI."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}
				UObject* Asset = AssetData.GetAsset();
				UEditorUtilityWidgetBlueprint* WidgetBlueprint = Cast<UEditorUtilityWidgetBlueprint>(Asset);
				UEditorUtilitySubsystem* UtilitySubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr;
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bAllowUserScriptExecution = false;
				bool bSpawnTab = true;
				Arguments->TryGetBoolField(TEXT("allow_user_script_execution"), bAllowUserScriptExecution);
				Arguments->TryGetBoolField(TEXT("spawn_tab"), bSpawnTab);
				const bool bReady = UtilitySubsystem && WidgetBlueprint;

				OutStructured->SetStringField(TEXT("operation"), TEXT("editor_utility_widget_run_safe"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetObjectField(TEXT("asset"), AssetDataToJsonDetailed(AssetData));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("allowUserScriptExecution"), bAllowUserScriptExecution);
				OutStructured->SetBoolField(TEXT("spawnTab"), bSpawnTab);
				OutStructured->SetBoolField(TEXT("subsystemAvailable"), UtilitySubsystem != nullptr);
				OutStructured->SetBoolField(TEXT("isEditorUtilityWidgetBlueprint"), WidgetBlueprint != nullptr);

				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), bReady ? TEXT("dry_run") : TEXT("blocked_not_widget_utility"));
					OutStructured->SetBoolField(TEXT("requires_execute"), bReady);
					OutSummary = FString::Printf(TEXT("Dry-run Editor Utility Widget safe-run for '%s': ready=%s."),
						*AssetData.GetObjectPathString(),
						bReady ? TEXT("true") : TEXT("false"));
					return true;
				}
				if (!bAllowUserScriptExecution)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked_requires_user_script_execution"));
					OutError = TEXT("editor_utility_widget_run_safe requires allow_user_script_execution=true when execute=true.");
					return false;
				}
				if (!bReady)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked_not_widget_utility"));
					OutError = TEXT("Editor Utility subsystem unavailable or asset is not an Editor Utility Widget Blueprint.");
					return false;
				}

				UEditorUtilityWidget* Widget = bSpawnTab ? UtilitySubsystem->SpawnAndRegisterTab(WidgetBlueprint) : nullptr;
				const bool bRan = !bSpawnTab ? UtilitySubsystem->TryRun(WidgetBlueprint) : Widget != nullptr;
				OutStructured->SetStringField(TEXT("status"), bRan ? TEXT("completed") : TEXT("failed"));
				OutStructured->SetBoolField(TEXT("ran"), bRan);
				if (Widget)
				{
					OutStructured->SetStringField(TEXT("widget"), Widget->GetPathName());
				}
				OutSummary = FString::Printf(TEXT("Editor Utility Widget safe-run %s for '%s'."), bRan ? TEXT("completed") : TEXT("failed"), *AssetData.GetObjectPathString());
				return bRan;
			}
		});

		Registry.Register({
			TEXT("asset_duplicate_v2"),
			TEXT("Concrete P2 safe asset duplicate. Defaults to dry-run; set execute=true to duplicate through EditorAssetSubsystem, verify Asset Registry readback, and optionally save."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Source asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("destination_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Full destination package path, e.g. '/Game/Folder/NewAsset'."))},
					{TEXT("destination_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for destination_asset_path."))},
					{TEXT("destination_folder"), FSololmcpSchemaBuilder::String(TEXT("Destination folder when using new_name."))},
					{TEXT("new_name"), FSololmcpSchemaBuilder::String(TEXT("Destination asset name when destination path is not provided."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save duplicated asset after execute. Default true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData SourceData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, SourceData, RequestedPath, OutError))
				{
					return false;
				}

				FString DestinationPath;
				if (!BuildPromotedDestinationPackagePath(Arguments, SourceData, DestinationPath, OutError))
				{
					return false;
				}

				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bSaveAsset = true;
				Arguments->TryGetBoolField(TEXT("save_asset"), bSaveAsset);
				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				const bool bDestinationExists = DestinationAssetExists(AssetRegistry, DestinationPath);

				OutStructured->SetStringField(TEXT("operation"), TEXT("duplicate"));
				OutStructured->SetStringField(TEXT("source"), SourceData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("sourceObjectPath"), SourceData.GetObjectPathString());
				OutStructured->SetStringField(TEXT("destination"), DestinationPath);
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("saveAsset"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("destinationExists"), bDestinationExists);

				if (bDestinationExists)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked_destination_exists"));
					OutStructured->SetBoolField(TEXT("ready"), false);
					OutSummary = FString::Printf(TEXT("Duplicate blocked: destination already exists: %s."), *DestinationPath);
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}

				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run duplicate plan: %s -> %s."), *SourceData.PackageName.ToString(), *DestinationPath);
					return true;
				}

				UObject* DuplicatedAsset = Context.Services.DuplicateAsset(SourceData.PackageName.ToString(), DestinationPath, OutError);
				if (!DuplicatedAsset)
				{
					return false;
				}
				if (bSaveAsset)
				{
					FString SaveError;
					if (!Context.Services.SaveAsset(DestinationPath, false, SaveError))
					{
						OutError = SaveError.IsEmpty() ? TEXT("Duplicate succeeded but SaveAsset failed.") : SaveError;
						return false;
					}
				}

				const FAssetData Readback = ResolveAssetDataByPath(AssetRegistry, DestinationPath);
				const bool bVerified = Readback.IsValid();
				OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("completed") : TEXT("failed_readback"));
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				OutStructured->SetStringField(TEXT("duplicatedObjectPath"), DuplicatedAsset->GetPathName());
				if (bVerified)
				{
					OutStructured->SetObjectField(TEXT("readback"), AssetDataToJsonDetailed(Readback));
				}
				else
				{
					OutError = FString::Printf(TEXT("Duplicated asset was not visible in Asset Registry: %s."), *DestinationPath);
					return false;
				}
				OutSummary = FString::Printf(TEXT("Duplicated asset: %s -> %s."), *SourceData.PackageName.ToString(), *DestinationPath);
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_rename_v2"),
			TEXT("Concrete P2 safe asset rename. Defaults to dry-run; set execute=true to rename through EditorAssetSubsystem and verify source/destination Asset Registry state."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Source asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("destination_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Full destination package path."))},
					{TEXT("destination_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for destination_asset_path."))},
					{TEXT("destination_folder"), FSololmcpSchemaBuilder::String(TEXT("Destination folder when using new_name."))},
					{TEXT("new_name"), FSololmcpSchemaBuilder::String(TEXT("New asset name when destination path is not provided."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save renamed asset after execute. Default true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData SourceData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, SourceData, RequestedPath, OutError))
				{
					return false;
				}

				FString DestinationPath;
				if (!BuildPromotedDestinationPackagePath(Arguments, SourceData, DestinationPath, OutError))
				{
					return false;
				}

				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bSaveAsset = true;
				Arguments->TryGetBoolField(TEXT("save_asset"), bSaveAsset);
				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				const bool bDestinationExists = DestinationAssetExists(AssetRegistry, DestinationPath);
				const bool bSamePath = SourceData.PackageName.ToString() == DestinationPath;

				OutStructured->SetStringField(TEXT("operation"), TEXT("rename"));
				OutStructured->SetStringField(TEXT("source"), SourceData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("sourceObjectPath"), SourceData.GetObjectPathString());
				OutStructured->SetStringField(TEXT("destination"), DestinationPath);
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("saveAsset"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("destinationExists"), bDestinationExists);
				OutStructured->SetBoolField(TEXT("samePath"), bSamePath);

				if (bSamePath || bDestinationExists)
				{
					OutStructured->SetStringField(TEXT("status"), bSamePath ? TEXT("blocked_same_path") : TEXT("blocked_destination_exists"));
					OutStructured->SetBoolField(TEXT("ready"), false);
					OutSummary = bSamePath
						? FString::Printf(TEXT("Rename blocked: source and destination are the same: %s."), *DestinationPath)
						: FString::Printf(TEXT("Rename blocked: destination already exists: %s."), *DestinationPath);
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}

				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run rename plan: %s -> %s."), *SourceData.PackageName.ToString(), *DestinationPath);
					return true;
				}

				if (!Context.Services.RenameAsset(SourceData.PackageName.ToString(), DestinationPath, OutError))
				{
					return false;
				}
				if (bSaveAsset)
				{
					FString SaveError;
					if (!Context.Services.SaveAsset(DestinationPath, false, SaveError))
					{
						OutError = SaveError.IsEmpty() ? TEXT("Rename succeeded but SaveAsset failed.") : SaveError;
						return false;
					}
				}

				const bool bSourceStillExists = DestinationAssetExists(AssetRegistry, SourceData.PackageName.ToString());
				const FAssetData Readback = ResolveAssetDataByPath(AssetRegistry, DestinationPath);
				const bool bVerified = !bSourceStillExists && Readback.IsValid();
				OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("completed") : TEXT("failed_readback"));
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				OutStructured->SetBoolField(TEXT("sourceStillExists"), bSourceStillExists);
				if (Readback.IsValid())
				{
					OutStructured->SetObjectField(TEXT("readback"), AssetDataToJsonDetailed(Readback));
				}
				if (!bVerified)
				{
					OutError = FString::Printf(TEXT("Rename verification failed: %s -> %s."), *SourceData.PackageName.ToString(), *DestinationPath);
					return false;
				}
				OutSummary = FString::Printf(TEXT("Renamed asset: %s -> %s."), *SourceData.PackageName.ToString(), *DestinationPath);
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_move_v2"),
			TEXT("Concrete P2 safe asset move. Defaults to dry-run; set execute=true to move to destination_folder or destination_asset_path and verify Asset Registry readback."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Source asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("destination_folder"), FSololmcpSchemaBuilder::String(TEXT("Destination folder, e.g. '/Game/NewFolder'."))},
					{TEXT("destination_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional full destination package path."))},
					{TEXT("destination_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for destination_asset_path."))},
					{TEXT("new_name"), FSololmcpSchemaBuilder::String(TEXT("Optional new name. Defaults to source asset name for move."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save moved asset after execute. Default true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData SourceData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, SourceData, RequestedPath, OutError))
				{
					return false;
				}

				FString DestinationPath;
				if (!BuildPromotedDestinationPackagePath(Arguments, SourceData, DestinationPath, OutError, true))
				{
					return false;
				}

				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bSaveAsset = true;
				Arguments->TryGetBoolField(TEXT("save_asset"), bSaveAsset);
				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				const bool bDestinationExists = DestinationAssetExists(AssetRegistry, DestinationPath);
				const bool bSamePath = SourceData.PackageName.ToString() == DestinationPath;

				OutStructured->SetStringField(TEXT("operation"), TEXT("move"));
				OutStructured->SetStringField(TEXT("source"), SourceData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("sourceObjectPath"), SourceData.GetObjectPathString());
				OutStructured->SetStringField(TEXT("destination"), DestinationPath);
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("saveAsset"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("destinationExists"), bDestinationExists);
				OutStructured->SetBoolField(TEXT("samePath"), bSamePath);

				if (bSamePath || bDestinationExists)
				{
					OutStructured->SetStringField(TEXT("status"), bSamePath ? TEXT("blocked_same_path") : TEXT("blocked_destination_exists"));
					OutStructured->SetBoolField(TEXT("ready"), false);
					OutSummary = bSamePath
						? FString::Printf(TEXT("Move blocked: source and destination are the same: %s."), *DestinationPath)
						: FString::Printf(TEXT("Move blocked: destination already exists: %s."), *DestinationPath);
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}

				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run move plan: %s -> %s."), *SourceData.PackageName.ToString(), *DestinationPath);
					return true;
				}

				if (!Context.Services.RenameAsset(SourceData.PackageName.ToString(), DestinationPath, OutError))
				{
					return false;
				}
				if (bSaveAsset)
				{
					FString SaveError;
					if (!Context.Services.SaveAsset(DestinationPath, false, SaveError))
					{
						OutError = SaveError.IsEmpty() ? TEXT("Move succeeded but SaveAsset failed.") : SaveError;
						return false;
					}
				}

				const bool bSourceStillExists = DestinationAssetExists(AssetRegistry, SourceData.PackageName.ToString());
				const FAssetData Readback = ResolveAssetDataByPath(AssetRegistry, DestinationPath);
				const bool bVerified = !bSourceStillExists && Readback.IsValid();
				OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("completed") : TEXT("failed_readback"));
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				OutStructured->SetBoolField(TEXT("sourceStillExists"), bSourceStillExists);
				if (Readback.IsValid())
				{
					OutStructured->SetObjectField(TEXT("readback"), AssetDataToJsonDetailed(Readback));
				}
				if (!bVerified)
				{
					OutError = FString::Printf(TEXT("Move verification failed: %s -> %s."), *SourceData.PackageName.ToString(), *DestinationPath);
					return false;
				}
				OutSummary = FString::Printf(TEXT("Moved asset: %s -> %s."), *SourceData.PackageName.ToString(), *DestinationPath);
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_editor_save_safe"),
			TEXT("Concrete P2 safe asset save. Defaults to dry-run; set execute=true to save the target asset and verify it remains loadable."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, only reports dirty/save plan."))},
					{TEXT("only_if_dirty"), FSololmcpSchemaBuilder::Boolean(TEXT("Only save dirty packages. Default true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}

				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				bool bOnlyIfDirty = true;
				Arguments->TryGetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
				UObject* LoadedAsset = AssetData.GetAsset();
				UPackage* Package = LoadedAsset ? LoadedAsset->GetOutermost() : FindPackage(nullptr, *AssetData.PackageName.ToString());
				const bool bDirtyBefore = Package ? Package->IsDirty() : false;

				OutStructured->SetStringField(TEXT("operation"), TEXT("save"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetStringField(TEXT("assetPath"), AssetData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("onlyIfDirty"), bOnlyIfDirty);
				OutStructured->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);

				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run save plan for '%s' (dirty=%s)."),
						*AssetData.PackageName.ToString(),
						bDirtyBefore ? TEXT("true") : TEXT("false"));
					return true;
				}

				if (!Context.Services.SaveAsset(AssetData.PackageName.ToString(), bOnlyIfDirty, OutError))
				{
					return false;
				}

				FString ReloadError;
				UObject* Reloaded = LoadAssetFlexible(AssetData.PackageName.ToString(), AssetData, RequestedPath, ReloadError);
				if (!Reloaded)
				{
					OutError = ReloadError.IsEmpty() ? TEXT("Save succeeded but reload verification failed.") : ReloadError;
					return false;
				}

				UPackage* PackageAfter = Reloaded->GetOutermost();
				OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
				OutStructured->SetBoolField(TEXT("verified"), true);
				OutStructured->SetBoolField(TEXT("dirtyAfter"), PackageAfter ? PackageAfter->IsDirty() : false);
				OutStructured->SetStringField(TEXT("reloadedObjectPath"), Reloaded->GetPathName());
				OutSummary = FString::Printf(TEXT("Saved asset safely: %s."), *AssetData.PackageName.ToString());
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_redirectors_fixup_v2"),
			TEXT("Concrete P2 redirector fixup. Defaults to dry-run; set execute=true to call AssetTools.FixupReferencers for redirectors under path_filter."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("path_filter"), FSololmcpSchemaBuilder::String(TEXT("Content root to scan. Default '/Game'."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, reports redirectors without modifying assets."))},
					{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum redirector rows to return. Default 200."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PathFilter = TEXT("/Game");
				Arguments->TryGetStringField(TEXT("path_filter"), PathFilter);
				PathFilter.TrimStartAndEndInline();
				if (PathFilter.IsEmpty())
				{
					PathFilter = TEXT("/Game");
				}
				if (!PathFilter.StartsWith(TEXT("/")))
				{
					OutError = FString::Printf(TEXT("path_filter must be a mounted content path, got '%s'."), *PathFilter);
					return false;
				}

				int32 MaxResults = 200;
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
				MaxResults = FMath::Clamp(MaxResults, 1, 1000);
				const bool bExecute = GetPromotedExecuteFlag(Arguments);

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				FARFilter Filter;
				Filter.bRecursivePaths = true;
				Filter.PackagePaths.Add(FName(*PathFilter));
				Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());

				TArray<FAssetData> RedirectorData;
				AssetRegistry.GetAssets(Filter, RedirectorData);
				TArray<TSharedPtr<FJsonValue>> Rows;
				TArray<UObjectRedirector*> Redirectors;
				for (int32 Index = 0; Index < RedirectorData.Num(); ++Index)
				{
					const FAssetData& Data = RedirectorData[Index];
					if (Index < MaxResults)
					{
						Rows.Add(MakeShared<FJsonValueObject>(AssetDataToJsonDetailed(Data)));
					}
					if (bExecute)
					{
						if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Data.GetAsset()))
						{
							Redirectors.Add(Redirector);
						}
					}
				}

				OutStructured->SetStringField(TEXT("operation"), TEXT("fixup_redirectors"));
				OutStructured->SetStringField(TEXT("pathFilter"), PathFilter);
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetArrayField(TEXT("redirectors"), Rows);
				OutStructured->SetNumberField(TEXT("found"), RedirectorData.Num());
				OutStructured->SetBoolField(TEXT("truncated"), RedirectorData.Num() > MaxResults);

				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), RedirectorData.Num() > 0);
					OutSummary = FString::Printf(TEXT("Dry-run redirector fixup under '%s': found %d redirectors."),
						*PathFilter,
						RedirectorData.Num());
					return true;
				}

				if (Redirectors.Num() > 0)
				{
					FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
					AssetToolsModule.Get().FixupReferencers(Redirectors);
				}

				TArray<FAssetData> Remaining;
				AssetRegistry.GetAssets(Filter, Remaining);
				OutStructured->SetStringField(TEXT("status"), Remaining.Num() == 0 ? TEXT("completed") : TEXT("partial"));
				OutStructured->SetNumberField(TEXT("attempted"), Redirectors.Num());
				OutStructured->SetNumberField(TEXT("remaining"), Remaining.Num());
				OutStructured->SetBoolField(TEXT("verified"), Remaining.Num() == 0);
				OutSummary = FString::Printf(TEXT("Redirector fixup under '%s': attempted %d, remaining %d."),
					*PathFilter,
					Redirectors.Num(),
					Remaining.Num());
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_collection_create"),
			TEXT("Concrete P2 asset collection create. Defaults to dry-run; set execute=true to create a Local/Private/Shared static collection."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("collection_name"), FSololmcpSchemaBuilder::String(TEXT("Collection name."))},
					{TEXT("share_type"), FSololmcpSchemaBuilder::String(TEXT("Local | Private | Shared. Default Local."))},
					{TEXT("storage_mode"), FSololmcpSchemaBuilder::String(TEXT("Static | Dynamic. Default Static."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FName CollectionName;
				if (!GetCollectionNameArgument(Arguments, CollectionName, OutError))
				{
					return false;
				}
				const ECollectionShareType::Type ShareType = ParseCollectionShareType(Arguments);
				const ECollectionStorageMode::Type StorageMode = ParseCollectionStorageMode(Arguments);
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				ICollectionManager& CollectionManager = FCollectionManagerModule::GetModule().Get();
				const bool bExists = CollectionManager.CollectionExists(CollectionName, ShareType);

				FText ValidationError;
				const bool bValidName = bExists || SomolIsValidCollectionName(CollectionManager, CollectionName.ToString(), ShareType, &ValidationError);
				OutStructured->SetStringField(TEXT("operation"), TEXT("collection_create"));
				OutStructured->SetStringField(TEXT("collectionName"), CollectionName.ToString());
				OutStructured->SetStringField(TEXT("shareType"), ECollectionShareType::ToString(ShareType));
				OutStructured->SetStringField(TEXT("storageMode"), StorageMode == ECollectionStorageMode::Dynamic ? TEXT("Dynamic") : TEXT("Static"));
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("exists"), bExists);
				OutStructured->SetBoolField(TEXT("validName"), bValidName);
				if (!ValidationError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("validationError"), ValidationError.ToString());
				}

				if (bExists)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("already_exists"));
					OutSummary = FString::Printf(TEXT("Collection already exists: %s (%s)."), *CollectionName.ToString(), ECollectionShareType::ToString(ShareType));
					return true;
				}
				if (!bValidName)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked_invalid_name"));
					OutSummary = FString::Printf(TEXT("Collection name is invalid: %s."), *ValidationError.ToString());
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run collection create: %s (%s)."), *CollectionName.ToString(), ECollectionShareType::ToString(ShareType));
					return true;
				}

				FText CreateError;
				const bool bCreated = SomolCreateCollection(CollectionManager, CollectionName, ShareType, StorageMode, &CreateError);
				if (!bCreated)
				{
					OutError = CreateError.IsEmpty() ? TEXT("CreateCollection failed.") : CreateError.ToString();
					return false;
				}
				FText SaveError;
				SomolSaveCollection(CollectionManager, CollectionName, ShareType, &SaveError);
				const bool bVerified = CollectionManager.CollectionExists(CollectionName, ShareType);
				OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("completed") : TEXT("failed_readback"));
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				if (!SaveError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("saveWarning"), SaveError.ToString());
				}
				if (!bVerified)
				{
					OutError = FString::Printf(TEXT("Collection create verification failed: %s."), *CollectionName.ToString());
					return false;
				}
				OutSummary = FString::Printf(TEXT("Created collection: %s (%s)."), *CollectionName.ToString(), ECollectionShareType::ToString(ShareType));
				return true;
			}
		});

		auto RegisterCollectionMembershipTool = [&Registry](const TCHAR* ToolName, const TCHAR* Operation)
		{
			Registry.Register({
				ToolName,
				FString::Printf(TEXT("Concrete P2 asset collection %s. Defaults to dry-run; set execute=true to mutate collection membership."), Operation),
				FSololmcpSchemaBuilder::Object(
					{
						{TEXT("collection_name"), FSololmcpSchemaBuilder::String(TEXT("Collection name."))},
						{TEXT("share_type"), FSololmcpSchemaBuilder::String(TEXT("Local | Private | Shared. Default Local."))},
						{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Asset path.")), TEXT("Assets to add/remove."))},
						{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Single asset path alternative."))},
						{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))},
						{TEXT("create_if_missing"), FSololmcpSchemaBuilder::Boolean(TEXT("For add only: create the collection when missing. Default false."))}
					},
					{}),

				[Operation](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
				{
					FName CollectionName;
					if (!GetCollectionNameArgument(Arguments, CollectionName, OutError))
					{
						return false;
					}
					TArray<FSoftObjectPath> ObjectPaths;
					TArray<TSharedPtr<FJsonValue>> ResolvedRows;
					TArray<TSharedPtr<FJsonValue>> MissingRows;
					if (!GetCollectionAssetPathsArgument(Arguments, ObjectPaths, ResolvedRows, MissingRows, OutError))
					{
						return false;
					}

					const bool bIsAdd = FCString::Stricmp(Operation, TEXT("add")) == 0;
					const bool bExecute = GetPromotedExecuteFlag(Arguments);
					bool bCreateIfMissing = false;
					Arguments->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);
					const ECollectionShareType::Type ShareType = ParseCollectionShareType(Arguments);
					ICollectionManager& CollectionManager = FCollectionManagerModule::GetModule().Get();
					const bool bExistsBefore = CollectionManager.CollectionExists(CollectionName, ShareType);
					const bool bBlockedMissingCollection = !bExistsBefore && !(bIsAdd && bCreateIfMissing);
					const bool bBlockedMissingAssets = MissingRows.Num() > 0;

					OutStructured->SetStringField(TEXT("operation"), bIsAdd ? TEXT("collection_add") : TEXT("collection_remove"));
					OutStructured->SetStringField(TEXT("collectionName"), CollectionName.ToString());
					OutStructured->SetStringField(TEXT("shareType"), ECollectionShareType::ToString(ShareType));
					OutStructured->SetBoolField(TEXT("execute"), bExecute);
					OutStructured->SetBoolField(TEXT("existsBefore"), bExistsBefore);
					OutStructured->SetBoolField(TEXT("createIfMissing"), bCreateIfMissing);
					OutStructured->SetArrayField(TEXT("resolvedAssets"), ResolvedRows);
					OutStructured->SetArrayField(TEXT("missingAssets"), MissingRows);
					OutStructured->SetNumberField(TEXT("resolvedCount"), ObjectPaths.Num());
					OutStructured->SetNumberField(TEXT("missingCount"), MissingRows.Num());

					if (bBlockedMissingCollection || bBlockedMissingAssets)
					{
						OutStructured->SetStringField(TEXT("status"), bBlockedMissingCollection ? TEXT("blocked_collection_missing") : TEXT("blocked_missing_assets"));
						OutStructured->SetBoolField(TEXT("ready"), false);
						OutSummary = bBlockedMissingCollection
							? FString::Printf(TEXT("Collection membership %s blocked: collection missing: %s."), Operation, *CollectionName.ToString())
							: FString::Printf(TEXT("Collection membership %s blocked: %d assets missing."), Operation, MissingRows.Num());
						if (bExecute)
						{
							OutError = OutSummary;
							return false;
						}
						return true;
					}
					if (!bExecute)
					{
						OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
						OutStructured->SetBoolField(TEXT("ready"), true);
						OutStructured->SetBoolField(TEXT("requires_execute"), true);
						OutSummary = FString::Printf(TEXT("Dry-run collection %s: %d assets in %s."), Operation, ObjectPaths.Num(), *CollectionName.ToString());
						return true;
					}

					if (!bExistsBefore && bIsAdd && bCreateIfMissing)
					{
						FText CreateError;
						if (!SomolCreateCollection(CollectionManager, CollectionName, ShareType, ECollectionStorageMode::Static, &CreateError))
						{
							OutError = CreateError.IsEmpty() ? TEXT("CreateCollection failed before membership add.") : CreateError.ToString();
							return false;
						}
					}

					int32 ChangedCount = 0;
					FText MembershipError;
					const bool bOk = bIsAdd
						? SomolAddToCollection(CollectionManager, CollectionName, ShareType, ObjectPaths, &ChangedCount, &MembershipError)
						: SomolRemoveFromCollection(CollectionManager, CollectionName, ShareType, ObjectPaths, &ChangedCount, &MembershipError);
					if (!bOk)
					{
						OutError = MembershipError.IsEmpty() ? FString::Printf(TEXT("Collection %s failed."), Operation) : MembershipError.ToString();
						return false;
					}

					FText SaveError;
					SomolSaveCollection(CollectionManager, CollectionName, ShareType, &SaveError);
					TArray<FSoftObjectPath> Readback;
					CollectionManager.GetObjectsInCollection(CollectionName, ShareType, Readback);
					TArray<TSharedPtr<FJsonValue>> ReadbackRows;
					for (const FSoftObjectPath& Path : Readback)
					{
						ReadbackRows.Add(MakeShared<FJsonValueString>(Path.ToString()));
					}

					OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
					OutStructured->SetNumberField(TEXT("changedCount"), ChangedCount);
					OutStructured->SetNumberField(TEXT("readbackCount"), Readback.Num());
					OutStructured->SetArrayField(TEXT("readbackAssets"), ReadbackRows);
					OutStructured->SetBoolField(TEXT("verified"), true);
					if (!SaveError.IsEmpty())
					{
						OutStructured->SetStringField(TEXT("saveWarning"), SaveError.ToString());
					}
					OutSummary = FString::Printf(TEXT("Collection %s completed: %d assets changed in %s."), Operation, ChangedCount, *CollectionName.ToString());
					return true;
				}
			});
		};

		RegisterCollectionMembershipTool(TEXT("asset_collection_add_items"), TEXT("add"));
		RegisterCollectionMembershipTool(TEXT("asset_collection_remove_items"), TEXT("remove"));

		Registry.Register({
			TEXT("asset_editor_open_v2"),
			TEXT("Concrete P2 asset editor open. Defaults to dry-run; set execute=true to open the asset editor and verify an editor instance is present."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				OutStructured->SetStringField(TEXT("operation"), TEXT("asset_editor_open"));
				OutStructured->SetStringField(TEXT("requestedPath"), RequestedPath);
				OutStructured->SetStringField(TEXT("assetPath"), AssetData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run asset editor open: %s."), *AssetData.GetObjectPathString());
					return true;
				}

				UObject* Asset = AssetData.GetAsset();
				if (!Asset)
				{
					OutError = FString::Printf(TEXT("Failed to load asset for editor open: %s."), *AssetData.GetObjectPathString());
					return false;
				}
				UAssetEditorSubsystem* EditorSubsystem = Context.Services.GetAssetEditorSubsystem(OutError);
				if (!EditorSubsystem)
				{
					return false;
				}
				const bool bOpened = EditorSubsystem->OpenEditorForAsset(Asset);
				IAssetEditorInstance* EditorInstance = EditorSubsystem->FindEditorForAsset(Asset, false);
				OutStructured->SetStringField(TEXT("status"), EditorInstance ? TEXT("completed") : TEXT("opened_without_instance_readback"));
				OutStructured->SetBoolField(TEXT("opened"), bOpened);
				OutStructured->SetBoolField(TEXT("editorInstanceFound"), EditorInstance != nullptr);
				OutSummary = FString::Printf(TEXT("Asset editor open requested for %s (opened=%s, instance=%s)."),
					*AssetData.GetObjectPathString(),
					bOpened ? TEXT("true") : TEXT("false"),
					EditorInstance ? TEXT("true") : TEXT("false"));
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_editor_focus_v2"),
			TEXT("Concrete P2 asset editor focus. Defaults to dry-run; set execute=true to focus an already open editor, optionally opening it if missing."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("open_if_missing"), FSololmcpSchemaBuilder::Boolean(TEXT("Open the asset editor if no editor is currently open. Default false."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}
				bool bOpenIfMissing = false;
				Arguments->TryGetBoolField(TEXT("open_if_missing"), bOpenIfMissing);
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				OutStructured->SetStringField(TEXT("operation"), TEXT("asset_editor_focus"));
				OutStructured->SetStringField(TEXT("assetPath"), AssetData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("openIfMissing"), bOpenIfMissing);
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run asset editor focus: %s."), *AssetData.GetObjectPathString());
					return true;
				}

				UObject* Asset = AssetData.GetAsset();
				if (!Asset)
				{
					OutError = FString::Printf(TEXT("Failed to load asset for editor focus: %s."), *AssetData.GetObjectPathString());
					return false;
				}
				UAssetEditorSubsystem* EditorSubsystem = Context.Services.GetAssetEditorSubsystem(OutError);
				if (!EditorSubsystem)
				{
					return false;
				}
				IAssetEditorInstance* EditorInstance = EditorSubsystem->FindEditorForAsset(Asset, true);
				bool bOpened = false;
				if (!EditorInstance && bOpenIfMissing)
				{
					bOpened = EditorSubsystem->OpenEditorForAsset(Asset);
					EditorInstance = EditorSubsystem->FindEditorForAsset(Asset, true);
				}
				OutStructured->SetStringField(TEXT("status"), EditorInstance ? TEXT("completed") : TEXT("not_open"));
				OutStructured->SetBoolField(TEXT("opened"), bOpened);
				OutStructured->SetBoolField(TEXT("focused"), EditorInstance != nullptr);
				OutSummary = EditorInstance
					? FString::Printf(TEXT("Focused asset editor for %s."), *AssetData.GetObjectPathString())
					: FString::Printf(TEXT("No asset editor is open for %s."), *AssetData.GetObjectPathString());
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_editor_close_v2"),
			TEXT("Concrete P2 asset editor close. Defaults to dry-run; set execute=true to close all editor instances for the target asset, optionally saving first."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset package or object path."))},
					{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
					{TEXT("save_before_close"), FSololmcpSchemaBuilder::Boolean(TEXT("Save asset before closing. Default false."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetData AssetData;
				FString RequestedPath;
				if (!ResolveAssetDataForPromotedTool(Arguments, AssetData, RequestedPath, OutError))
				{
					return false;
				}
				bool bSaveBeforeClose = false;
				Arguments->TryGetBoolField(TEXT("save_before_close"), bSaveBeforeClose);
				const bool bExecute = GetPromotedExecuteFlag(Arguments);
				OutStructured->SetStringField(TEXT("operation"), TEXT("asset_editor_close"));
				OutStructured->SetStringField(TEXT("assetPath"), AssetData.PackageName.ToString());
				OutStructured->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("saveBeforeClose"), bSaveBeforeClose);
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run asset editor close: %s."), *AssetData.GetObjectPathString());
					return true;
				}

				UObject* Asset = AssetData.GetAsset();
				if (!Asset)
				{
					OutError = FString::Printf(TEXT("Failed to load asset for editor close: %s."), *AssetData.GetObjectPathString());
					return false;
				}
				if (bSaveBeforeClose)
				{
					FString SaveError;
					if (!Context.Services.SaveAsset(AssetData.PackageName.ToString(), true, SaveError))
					{
						OutError = SaveError.IsEmpty() ? TEXT("Save before close failed.") : SaveError;
						return false;
					}
				}
				UAssetEditorSubsystem* EditorSubsystem = Context.Services.GetAssetEditorSubsystem(OutError);
				if (!EditorSubsystem)
				{
					return false;
				}
				const int32 ClosedCount = EditorSubsystem->CloseAllEditorsForAsset(Asset);
				OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
				OutStructured->SetNumberField(TEXT("closedCount"), ClosedCount);
				OutStructured->SetBoolField(TEXT("closedAny"), ClosedCount > 0);
				OutSummary = FString::Printf(TEXT("Closed %d editor(s) for %s."), ClosedCount, *AssetData.GetObjectPathString());
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_batch_import_task_plan"),
			TEXT("Concrete P2 batch import planner. Builds validated UAssetImportTask-style rows without importing. Use before asset_import_task_execute_safe."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("source_files"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Source file to import.")), TEXT("Source files."))},
					{TEXT("source_file"), FSololmcpSchemaBuilder::String(TEXT("Single source file alternative."))},
					{TEXT("destination_path"), FSololmcpSchemaBuilder::String(TEXT("Destination content folder, e.g. /Game/Imported."))},
					{TEXT("destination_names"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Optional destination asset name.")), TEXT("Optional per-source destination names."))},
					{TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean(TEXT("Would replace existing assets. Default false."))},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Would save after import. Default true."))},
					{TEXT("automated"), FSololmcpSchemaBuilder::Boolean(TEXT("Would avoid dialogs. Default true."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> SourceFiles;
				if (!TryGetStringArrayArgument(Arguments, TEXT("source_files"), SourceFiles))
				{
					FString SourceFile;
					Arguments->TryGetStringField(TEXT("source_file"), SourceFile);
					SourceFile.TrimStartAndEndInline();
					if (!SourceFile.IsEmpty())
					{
						SourceFiles.Add(SourceFile);
					}
				}
				FString DestinationPath;
				Arguments->TryGetStringField(TEXT("destination_path"), DestinationPath);
				DestinationPath.TrimStartAndEndInline();
				if (SourceFiles.Num() == 0 || DestinationPath.IsEmpty())
				{
					OutError = TEXT("Missing source_files/source_file or destination_path.");
					return false;
				}
				if (!DestinationPath.StartsWith(TEXT("/")))
				{
					OutError = FString::Printf(TEXT("destination_path must be a mounted content path, got '%s'."), *DestinationPath);
					return false;
				}

				TArray<FString> DestinationNames;
				TryGetStringArrayArgument(Arguments, TEXT("destination_names"), DestinationNames);
				bool bReplaceExisting = false;
				bool bSave = true;
				bool bAutomated = true;
				Arguments->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
				Arguments->TryGetBoolField(TEXT("save"), bSave);
				Arguments->TryGetBoolField(TEXT("automated"), bAutomated);

				TArray<TSharedPtr<FJsonValue>> Rows;
				TArray<TSharedPtr<FJsonValue>> Problems;
				TArray<FString> PlannedPackagePaths;
				BuildImportTaskPlanRows(SourceFiles, DestinationPath, DestinationNames, Rows, Problems, PlannedPackagePaths);

				OutStructured->SetStringField(TEXT("operation"), TEXT("batch_import_plan"));
				OutStructured->SetStringField(TEXT("destinationPath"), DestinationPath);
				OutStructured->SetBoolField(TEXT("replaceExisting"), bReplaceExisting);
				OutStructured->SetBoolField(TEXT("save"), bSave);
				OutStructured->SetBoolField(TEXT("automated"), bAutomated);
				OutStructured->SetArrayField(TEXT("tasks"), Rows);
				OutStructured->SetArrayField(TEXT("problems"), Problems);
				OutStructured->SetNumberField(TEXT("taskCount"), Rows.Num());
				OutStructured->SetNumberField(TEXT("problemCount"), Problems.Num());
				OutStructured->SetStringField(TEXT("status"), Problems.Num() == 0 ? TEXT("ready") : TEXT("blocked"));
				OutSummary = FString::Printf(TEXT("Import task plan: %d task(s), %d problem(s)."), Rows.Num(), Problems.Num());
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_import_task_execute_safe"),
			TEXT("Concrete P2 safe batch import executor. Defaults to dry-run; set execute=true to run UAssetImportTask imports after source and destination validation."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("source_files"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Source file to import.")), TEXT("Source files."))},
					{TEXT("source_file"), FSololmcpSchemaBuilder::String(TEXT("Single source file alternative."))},
					{TEXT("destination_path"), FSololmcpSchemaBuilder::String(TEXT("Destination content folder, e.g. /Game/Imported."))},
					{TEXT("destination_names"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Optional destination asset name.")), TEXT("Optional per-source destination names."))},
					{TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean(TEXT("Replace existing destination assets. Default false."))},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after import. Default true."))},
					{TEXT("automated"), FSololmcpSchemaBuilder::Boolean(TEXT("Avoid dialogs. Default true."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a dry-run import plan only."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> SourceFiles;
				if (!TryGetStringArrayArgument(Arguments, TEXT("source_files"), SourceFiles))
				{
					FString SourceFile;
					Arguments->TryGetStringField(TEXT("source_file"), SourceFile);
					SourceFile.TrimStartAndEndInline();
					if (!SourceFile.IsEmpty())
					{
						SourceFiles.Add(SourceFile);
					}
				}
				FString DestinationPath;
				Arguments->TryGetStringField(TEXT("destination_path"), DestinationPath);
				DestinationPath.TrimStartAndEndInline();
				if (SourceFiles.Num() == 0 || DestinationPath.IsEmpty())
				{
					OutError = TEXT("Missing source_files/source_file or destination_path.");
					return false;
				}
				if (!DestinationPath.StartsWith(TEXT("/Game")))
				{
					OutError = FString::Printf(TEXT("asset_import_task_execute_safe currently requires a /Game destination, got '%s'."), *DestinationPath);
					return false;
				}

				TArray<FString> DestinationNames;
				TryGetStringArrayArgument(Arguments, TEXT("destination_names"), DestinationNames);
				bool bReplaceExisting = false;
				bool bSave = true;
				bool bAutomated = true;
				Arguments->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
				Arguments->TryGetBoolField(TEXT("save"), bSave);
				Arguments->TryGetBoolField(TEXT("automated"), bAutomated);
				const bool bExecute = GetPromotedExecuteFlag(Arguments);

				TArray<TSharedPtr<FJsonValue>> Rows;
				TArray<TSharedPtr<FJsonValue>> Problems;
				TArray<FString> PlannedPackagePaths;
				BuildImportTaskPlanRows(SourceFiles, DestinationPath, DestinationNames, Rows, Problems, PlannedPackagePaths);
				OutStructured->SetStringField(TEXT("operation"), TEXT("batch_import_execute"));
				OutStructured->SetStringField(TEXT("destinationPath"), DestinationPath);
				OutStructured->SetBoolField(TEXT("execute"), bExecute);
				OutStructured->SetBoolField(TEXT("replaceExisting"), bReplaceExisting);
				OutStructured->SetBoolField(TEXT("save"), bSave);
				OutStructured->SetBoolField(TEXT("automated"), bAutomated);
				OutStructured->SetArrayField(TEXT("tasks"), Rows);
				OutStructured->SetArrayField(TEXT("problems"), Problems);
				OutStructured->SetNumberField(TEXT("taskCount"), Rows.Num());
				OutStructured->SetNumberField(TEXT("problemCount"), Problems.Num());

				if (Problems.Num() > 0)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked"));
					OutSummary = FString::Printf(TEXT("Import blocked: %d problem(s)."), Problems.Num());
					if (bExecute)
					{
						OutError = OutSummary;
						return false;
					}
					return true;
				}
				if (!bExecute)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
					OutStructured->SetBoolField(TEXT("ready"), true);
					OutStructured->SetBoolField(TEXT("requires_execute"), true);
					OutSummary = FString::Printf(TEXT("Dry-run import execute plan: %d task(s)."), Rows.Num());
					return true;
				}

				TArray<UAssetImportTask*> Tasks;
				for (int32 Index = 0; Index < SourceFiles.Num(); ++Index)
				{
					UAssetImportTask* Task = NewObject<UAssetImportTask>(GetTransientPackage());
					Task->Filename = SourceFiles[Index];
					Task->DestinationPath = DestinationPath;
					if (DestinationNames.IsValidIndex(Index))
					{
						Task->DestinationName = DestinationNames[Index];
					}
					Task->bAutomated = bAutomated;
					Task->bReplaceExisting = bReplaceExisting;
					Task->bReplaceExistingSettings = bReplaceExisting;
					Task->bSave = bSave;
					Task->bAsync = false;
					Tasks.Add(Task);
				}

				FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
				AssetToolsModule.Get().ImportAssetTasks(Tasks);

				TArray<TSharedPtr<FJsonValue>> ImportedObjects;
				TArray<TSharedPtr<FJsonValue>> ImportedPaths;
				TArray<TSharedPtr<FJsonValue>> FailedFiles;
				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				for (UAssetImportTask* Task : Tasks)
				{
					int32 BeforeCount = ImportedObjects.Num();
					for (UObject* Object : Task->GetObjects())
					{
						if (Object)
						{
							ImportedObjects.Add(MakeShared<FJsonValueObject>(FSololmcpEditorServices::MakeObjectReference(Object)));
							ImportedPaths.Add(MakeShared<FJsonValueString>(Object->GetPathName()));
						}
					}
					for (const FString& ImportedPath : Task->ImportedObjectPaths)
					{
						ImportedPaths.Add(MakeShared<FJsonValueString>(ImportedPath));
						ResolveAssetDataByPath(AssetRegistry, ImportedPath);
					}
					if (ImportedObjects.Num() == BeforeCount && Task->ImportedObjectPaths.Num() == 0)
					{
						FailedFiles.Add(MakeShared<FJsonValueString>(Task ? Task->Filename : TEXT("(null task)")));
					}
				}

				OutStructured->SetStringField(TEXT("status"), FailedFiles.Num() == 0 ? TEXT("completed") : TEXT("partial"));
				OutStructured->SetArrayField(TEXT("importedObjects"), ImportedObjects);
				OutStructured->SetArrayField(TEXT("importedObjectPaths"), ImportedPaths);
				OutStructured->SetArrayField(TEXT("failedFiles"), FailedFiles);
				OutStructured->SetNumberField(TEXT("importedObjectCount"), ImportedObjects.Num());
				OutStructured->SetNumberField(TEXT("importedPathCount"), ImportedPaths.Num());
				OutStructured->SetNumberField(TEXT("failedCount"), FailedFiles.Num());
				if (ImportedObjects.Num() == 0 && ImportedPaths.Num() == 0)
				{
					OutError = TEXT("Import executed but produced no imported objects or paths.");
					return false;
				}
				OutSummary = FString::Printf(TEXT("Import executed: %d object(s), %d path(s), %d failed file(s)."),
					ImportedObjects.Num(),
					ImportedPaths.Num(),
					FailedFiles.Num());
				return true;
			}
		});

		Registry.Register({
			TEXT("asset_import_receipt_validate"),
			TEXT("Concrete P2 import receipt validator. Checks imported paths against Asset Registry and optional source files on disk."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("imported_object_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Imported object or package path.")), TEXT("Imported paths."))},
					{TEXT("imported_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Alias for imported_object_paths.")), TEXT("Imported paths."))},
					{TEXT("source_files"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Original source file.")), TEXT("Optional source files."))},
					{TEXT("expected_min_count"), FSololmcpSchemaBuilder::Integer(TEXT("Minimum imported assets expected. Default 1."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> ImportedPaths;
				if (!TryGetStringArrayArgument(Arguments, TEXT("imported_object_paths"), ImportedPaths))
				{
					TryGetStringArrayArgument(Arguments, TEXT("imported_paths"), ImportedPaths);
				}
				TArray<FString> SourceFiles;
				TryGetStringArrayArgument(Arguments, TEXT("source_files"), SourceFiles);
				int32 ExpectedMinCount = 1;
				Arguments->TryGetNumberField(TEXT("expected_min_count"), ExpectedMinCount);
				ExpectedMinCount = FMath::Max(ExpectedMinCount, 0);

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
				TArray<TSharedPtr<FJsonValue>> ImportedRows;
				TArray<TSharedPtr<FJsonValue>> MissingImportedRows;
				for (const FString& Path : ImportedPaths)
				{
					FAssetData AssetData = ResolveAssetDataByPath(AssetRegistry, Path);
					if (AssetData.IsValid())
					{
						ImportedRows.Add(MakeShared<FJsonValueObject>(AssetDataToJsonDetailed(AssetData)));
					}
					else
					{
						TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
						Missing->SetStringField(TEXT("path"), Path);
						Missing->SetStringField(TEXT("reason"), TEXT("not_found_in_asset_registry"));
						MissingImportedRows.Add(MakeShared<FJsonValueObject>(Missing));
					}
				}

				TArray<TSharedPtr<FJsonValue>> MissingSourceRows;
				for (const FString& SourceFile : SourceFiles)
				{
					if (!FPaths::FileExists(SourceFile))
					{
						TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
						Missing->SetStringField(TEXT("sourceFile"), SourceFile);
						Missing->SetStringField(TEXT("reason"), TEXT("source_file_missing"));
						MissingSourceRows.Add(MakeShared<FJsonValueObject>(Missing));
					}
				}

				const bool bCountOk = ImportedRows.Num() >= ExpectedMinCount;
				const bool bValid = bCountOk && MissingImportedRows.Num() == 0 && MissingSourceRows.Num() == 0;
				OutStructured->SetStringField(TEXT("status"), bValid ? TEXT("valid") : TEXT("invalid"));
				OutStructured->SetBoolField(TEXT("valid"), bValid);
				OutStructured->SetNumberField(TEXT("expectedMinCount"), ExpectedMinCount);
				OutStructured->SetNumberField(TEXT("resolvedImportedCount"), ImportedRows.Num());
				OutStructured->SetNumberField(TEXT("missingImportedCount"), MissingImportedRows.Num());
				OutStructured->SetNumberField(TEXT("missingSourceCount"), MissingSourceRows.Num());
				OutStructured->SetArrayField(TEXT("importedAssets"), ImportedRows);
				OutStructured->SetArrayField(TEXT("missingImportedPaths"), MissingImportedRows);
				OutStructured->SetArrayField(TEXT("missingSourceFiles"), MissingSourceRows);
				OutSummary = FString::Printf(TEXT("Import receipt validation %s: %d imported assets, %d missing imported paths, %d missing source files."),
					bValid ? TEXT("passed") : TEXT("failed"),
					ImportedRows.Num(),
					MissingImportedRows.Num(),
					MissingSourceRows.Num());
				return bValid;
			}
		});
	}

} // namespace UE::SOMOLMCP
