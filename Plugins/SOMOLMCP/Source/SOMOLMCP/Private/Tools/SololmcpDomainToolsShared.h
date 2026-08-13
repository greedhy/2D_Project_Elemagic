// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

// Shared helpers lifted out of SololmcpDomainTools.cpp.
//
// That file is ~45,000 lines in one translation unit and has produced
// non-deterministic MSVC internal compiler errors. Splitting it starts here:
// the helpers move first, unchanged, so the register functions can follow one at
// a time into their own .cpp files.
//
// This header depends on the include block at the top of SololmcpDomainTools.cpp
// and must be included after it, not standalone.

// The WorldPartition error collector below dereferences UDataLayerInstanceWithAsset,
// so the include belongs here rather than in each including .cpp: the seven callers
// no longer share one preamble, and six of them carrying it while the seventh did not
// is exactly how it went missing.
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"

namespace UE::SOMOLMCP
{

	// Resolve a RigVM controller across the UE 5.3 / 5.4+ API split: 5.4 introduced
	// URigVMEditorBlueprintLibrary, while 5.3 exposes the controller as a member of
	// the blueprint itself. Same result, different route.
	static URigVMController* ResolveRigVMControllerCompat(URigVMBlueprint* Rig)
	{
		if (Rig == nullptr)
		{
			return nullptr;
		}
#if SOMOLMCP_DOMAIN_HAS_RIGVM_EDITOR_BP_LIBRARY
		return URigVMEditorBlueprintLibrary::GetController(Rig);
#else
		return Rig->GetController();
#endif
	}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 5
	using FSomolEditorGraphPosition = FVector2D;
	#else
	using FSomolEditorGraphPosition = FVector2f;
	#endif
	namespace
	{
		TSharedRef<FJsonObject> VectorSchema(const FString& Description = FString())
		{
			return FSololmcpSchemaBuilder::Object(
				{
					{TEXT("x"), FSololmcpSchemaBuilder::Number()},
					{TEXT("y"), FSololmcpSchemaBuilder::Number()},
					{TEXT("z"), FSololmcpSchemaBuilder::Number()}
				},
				{TEXT("x"), TEXT("y"), TEXT("z")},
				Description);
		}

		TSharedRef<FJsonObject> RotatorSchema(const FString& Description = FString())
		{
			return FSololmcpSchemaBuilder::Object(
				{
					{TEXT("pitch"), FSololmcpSchemaBuilder::Number()},
					{TEXT("yaw"), FSololmcpSchemaBuilder::Number()},
					{TEXT("roll"), FSololmcpSchemaBuilder::Number()}
				},
				{TEXT("pitch"), TEXT("yaw"), TEXT("roll")},
				Description);
		}

		TSharedRef<FJsonObject> ColorSchema(const FString& Description = FString())
		{
			return FSololmcpSchemaBuilder::Object(
				{
					{TEXT("r"), FSololmcpSchemaBuilder::Number()},
					{TEXT("g"), FSololmcpSchemaBuilder::Number()},
					{TEXT("b"), FSololmcpSchemaBuilder::Number()},
					{TEXT("a"), FSololmcpSchemaBuilder::Number()}
				},
				{TEXT("r"), TEXT("g"), TEXT("b")},
				Description);
		}

		TSharedRef<FJsonObject> LinearColorToJson(const FLinearColor& Color);
		TSharedRef<FJsonObject> VectorToJson(const FVector& Vector);

		TSharedRef<FJsonObject> TransformSchema()
		{
			return FSololmcpSchemaBuilder::Object(
				{
					{TEXT("location"), VectorSchema()},
					{TEXT("rotation"), RotatorSchema()},
					{TEXT("scale"), VectorSchema()}
				});
		}

		FString CombinePackageAssetPath(const FString& PackagePath, const FString& AssetName)
		{
			return PackagePath.EndsWith(TEXT("/")) ? PackagePath + AssetName : PackagePath + TEXT("/") + AssetName;
		}

		struct FSololmcpManagedStreamingSourceProvider final : IWorldPartitionStreamingSourceProvider
		{
			FString SourceId;
			TWeakObjectPtr<UWorld> World;
			FWorldPartitionStreamingSource Source;

			virtual bool GetStreamingSource(FWorldPartitionStreamingSource& StreamingSource) const override
			{
				if (!World.IsValid())
				{
					return false;
				}

				StreamingSource = Source;
				return true;
			}

			virtual const UObject* GetStreamingSourceOwner() const override
			{
				return World.Get();
			}
		};

		TArray<TSharedPtr<FSololmcpManagedStreamingSourceProvider>>& GetManagedStreamingSourceProviders()
		{
			static TArray<TSharedPtr<FSololmcpManagedStreamingSourceProvider>> Providers;
			Providers.RemoveAll([](const TSharedPtr<FSololmcpManagedStreamingSourceProvider>& Provider)
			{
				return !Provider.IsValid() || !Provider->World.IsValid();
			});
			return Providers;
		}

		FString StreamingSourceTargetStateToString(const EStreamingSourceTargetState TargetState)
		{
			return FString(GetStreamingSourceTargetStateName(TargetState)).ToLower();
		}

		bool TryParseStreamingSourceTargetState(const FString& Value, EStreamingSourceTargetState& OutTargetState)
		{
			if (Value.Equals(TEXT("loaded"), ESearchCase::IgnoreCase))
			{
				OutTargetState = EStreamingSourceTargetState::Loaded;
				return true;
			}
			if (Value.Equals(TEXT("activated"), ESearchCase::IgnoreCase))
			{
				OutTargetState = EStreamingSourceTargetState::Activated;
				return true;
			}
			return false;
		}

		bool TryParseStreamingSourcePriority(const FString& Value, EStreamingSourcePriority& OutPriority)
		{
			if (Value.Equals(TEXT("highest"), ESearchCase::IgnoreCase))
			{
				OutPriority = EStreamingSourcePriority::Highest;
				return true;
			}
			if (Value.Equals(TEXT("high"), ESearchCase::IgnoreCase))
			{
				OutPriority = EStreamingSourcePriority::High;
				return true;
			}
			if (Value.Equals(TEXT("normal"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("default"), ESearchCase::IgnoreCase))
			{
				OutPriority = EStreamingSourcePriority::Normal;
				return true;
			}
			if (Value.Equals(TEXT("low"), ESearchCase::IgnoreCase))
			{
				OutPriority = EStreamingSourcePriority::Low;
				return true;
			}
			if (Value.Equals(TEXT("lowest"), ESearchCase::IgnoreCase))
			{
				OutPriority = EStreamingSourcePriority::Lowest;
				return true;
			}
			return false;
		}

		bool TryParseStreamingSourceTargetBehavior(const FString& Value, EStreamingSourceTargetBehavior& OutTargetBehavior)
		{
			if (Value.Equals(TEXT("include"), ESearchCase::IgnoreCase))
			{
				OutTargetBehavior = EStreamingSourceTargetBehavior::Include;
				return true;
			}
			if (Value.Equals(TEXT("exclude"), ESearchCase::IgnoreCase))
			{
				OutTargetBehavior = EStreamingSourceTargetBehavior::Exclude;
				return true;
			}
			return false;
		}

		ADirectionalLight* FindFirstDirectionalLight(UWorld* World)
		{
			if (!World)
			{
				return nullptr;
			}
			ADirectionalLight* Fallback = nullptr;
			for (TActorIterator<ADirectionalLight> It(World); It; ++It)
			{
				ADirectionalLight* Light = *It;
				if (!Light)
				{
					continue;
				}
				if (Light->GetActorLabel() == TEXT("SOM_Daylight_Sun"))
				{
					return Light;
				}
				if (!Fallback)
				{
					Fallback = Light;
				}
			}
			return Fallback;
		}

		UWorldPartitionSubsystem* GetCurrentEditorWorldPartitionSubsystem(FString& OutError)
		{
			if (!GEditor)
			{
				OutError = TEXT("Editor is unavailable.");
				return nullptr;
			}

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				OutError = TEXT("Editor world is unavailable.");
				return nullptr;
			}

			UWorldPartitionSubsystem* WorldPartitionSubsystem = World->GetSubsystem<UWorldPartitionSubsystem>();
			if (!WorldPartitionSubsystem)
			{
				OutError = TEXT("WorldPartitionSubsystem is unavailable.");
			}
			return WorldPartitionSubsystem;
		}

		FString ObjectNameFromPath(const FString& Path)
		{
			FString Left;
			FString Right;
			if (Path.Split(TEXT("/"), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				return Right;
			}
			return Path;
		}

		FString PythonQuote(const FString& Value)
		{
			FString Escaped = Value;
			Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
			Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
			Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
			return FString::Printf(TEXT("'%s'"), *Escaped);
		}

		FString PythonStringListLiteral(const TArray<FString>& Values)
		{
			TArray<FString> Items;
			for (const FString& Value : Values)
			{
				Items.Add(PythonQuote(Value));
			}
			return FString::Printf(TEXT("[%s]"), *FString::Join(Items, TEXT(", ")));
		}

		FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
		{
			FString Output;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
			FJsonSerializer::Serialize(Object, Writer);
			Writer->Close();
			return Output;
		}

		struct FWidgetClipboardEntry
		{
			FString SourceAssetPath;
			TArray<FString> WidgetNames;
		};

		FWidgetClipboardEntry GWidgetClipboard;

		bool RemoveWidgetFromTree(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget, FString& OutError)
		{
			if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !Widget)
			{
				OutError = TEXT("Invalid widget blueprint or widget.");
				return false;
			}
			if (WidgetBlueprint->WidgetTree->RootWidget == Widget)
			{
				WidgetBlueprint->WidgetTree->RootWidget = nullptr;
				return true;
			}
			if (UPanelWidget* Parent = Widget->GetParent())
			{
				return Parent->RemoveChild(Widget);
			}
			OutError = TEXT("Widget has no removable parent and is not the root widget.");
			return false;
		}

		bool InsertWidgetIntoTree(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget, UPanelWidget* Parent, int32 ChildIndex, FString& OutError)
		{
			if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !Widget)
			{
				OutError = TEXT("Invalid widget blueprint or widget.");
				return false;
			}
			if (!Parent)
			{
				WidgetBlueprint->WidgetTree->RootWidget = Widget;
				return true;
			}
			if (ChildIndex >= 0)
			{
				return Parent->InsertChildAt(ChildIndex, Widget) != nullptr;
			}
			return Parent->AddChild(Widget) != nullptr;
		}

		UWidget* DuplicateWidgetForBlueprint(UWidgetBlueprint* WidgetBlueprint, UWidget* SourceWidget, const FString& NewName, FString& OutError)
		{
			if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !SourceWidget)
			{
				OutError = TEXT("Invalid widget duplication arguments.");
				return nullptr;
			}
			UWidget* DuplicatedWidget = DuplicateObject<UWidget>(SourceWidget, WidgetBlueprint->WidgetTree, *NewName);
			if (!DuplicatedWidget)
			{
				OutError = TEXT("Failed to duplicate widget.");
			}
			return DuplicatedWidget;
		}

		bool ResolveWidgetBlueprintAndWidget(FSololmcpEditorServices& Services, const FString& AssetPath, const FString& WidgetName, UWidgetBlueprint*& OutWidgetBlueprint, UWidget*& OutWidget, FString& OutError)
		{
			OutWidgetBlueprint = Cast<UWidgetBlueprint>(Services.LoadAsset(AssetPath, OutError));
			if (!OutWidgetBlueprint || !OutWidgetBlueprint->WidgetTree)
			{
				OutError = TEXT("Asset is not a widget blueprint.");
				return false;
			}
			OutWidget = OutWidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
			if (!OutWidget)
			{
				OutError = TEXT("Widget was not found in widget tree.");
				return false;
			}
			return true;
		}

		enum class EUmgAnimationTrackKind : uint8
		{
			Unsupported,
			Float,
			Transform2D
		};

		FString GetUmgAnimationCompileOrRefreshHint()
		{
			return TEXT("Run blueprint_refresh_all_nodes, compile diagnostics, and preview/readback before delivery.");
		}

		FString NormalizeUmgAnimationToken(FString Value)
		{
			Value = Value.TrimStartAndEnd().ToLower();
			Value.ReplaceInline(TEXT("-"), TEXT("_"));
			Value.ReplaceInline(TEXT("."), TEXT("_"));
			return Value;
		}

		EUmgAnimationTrackKind ResolveUmgAnimationTrackKind(const FString& TrackType)
		{
			const FString Normalized = NormalizeUmgAnimationToken(TrackType.IsEmpty() ? TEXT("float") : TrackType);
			if (Normalized == TEXT("float") || Normalized == TEXT("render_opacity") || Normalized == TEXT("opacity"))
			{
				return EUmgAnimationTrackKind::Float;
			}
			if (Normalized == TEXT("2d_transform") || Normalized == TEXT("render_transform") || Normalized == TEXT("transform"))
			{
				return EUmgAnimationTrackKind::Transform2D;
			}
			return EUmgAnimationTrackKind::Unsupported;
		}

		FString UmgAnimationTrackKindToString(EUmgAnimationTrackKind Kind)
		{
			switch (Kind)
			{
			case EUmgAnimationTrackKind::Float:
				return TEXT("float");
			case EUmgAnimationTrackKind::Transform2D:
				return TEXT("2d_transform");
			default:
				return TEXT("unsupported");
			}
		}

		UWidgetAnimation* FindUmgAnimationByName(UWidgetBlueprint* WidgetBlueprint, const FString& AnimationName)
		{
			if (!WidgetBlueprint)
			{
				return nullptr;
			}
			for (UWidgetAnimation* Anim : WidgetBlueprint->Animations)
			{
				if (!Anim)
				{
					continue;
				}
				if (Anim->GetName() == AnimationName || Anim->GetFName() == FName(*AnimationName) || Anim->GetDisplayLabel() == AnimationName)
				{
					return Anim;
				}
			}
			return SOMOLMCP_FIND_OBJECT_EXACT(UWidgetAnimation, WidgetBlueprint, *AnimationName);
		}

		const FWidgetAnimationBinding* FindUmgAnimationBindingByGuid(const UWidgetAnimation* Animation, const FGuid& Guid)
		{
			if (!Animation)
			{
				return nullptr;
			}
			for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
			{
				if (Binding.AnimationGuid == Guid)
				{
					return &Binding;
				}
			}
			return nullptr;
		}

		int32 CountMovieSceneSectionKeyframes(const UMovieSceneSection* Section)
		{
			if (!Section)
			{
				return 0;
			}
			int32 KeyframeCount = 0;
			const FMovieSceneChannelProxy& ChannelProxy = const_cast<UMovieSceneSection*>(Section)->GetChannelProxy();
			for (const FMovieSceneChannelEntry& Entry : ChannelProxy.GetAllEntries())
			{
				for (FMovieSceneChannel* Channel : Entry.GetChannels())
				{
					if (Channel)
					{
						KeyframeCount += Channel->GetNumKeys();
					}
				}
			}
			return KeyframeCount;
		}

		int32 CountMovieSceneTrackKeyframes(const UMovieSceneTrack* Track)
		{
			if (!Track)
			{
				return 0;
			}
			int32 KeyframeCount = 0;
			for (UMovieSceneSection* Section : Track->GetAllSections())
			{
				KeyframeCount += CountMovieSceneSectionKeyframes(Section);
			}
			return KeyframeCount;
		}

		void CountUmgAnimationTracksAndKeyframes(const UWidgetAnimation* Animation, int32& OutTrackCount, int32& OutKeyframeCount)
		{
			OutTrackCount = 0;
			OutKeyframeCount = 0;
			const UMovieScene* MovieScene = Animation ? Animation->MovieScene : nullptr;
			if (!MovieScene)
			{
				return;
			}
			for (UMovieSceneTrack* Track : MovieScene->GetTracks())
			{
				if (Track)
				{
					++OutTrackCount;
					OutKeyframeCount += CountMovieSceneTrackKeyframes(Track);
				}
			}
			for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
			{
				for (UMovieSceneTrack* Track : Binding.GetTracks())
				{
					if (Track)
					{
						++OutTrackCount;
						OutKeyframeCount += CountMovieSceneTrackKeyframes(Track);
					}
				}
			}
		}

		TSharedRef<FJsonObject> MakeUmgAnimationTrackJson(const UWidgetAnimation* Animation, UMovieSceneTrack* Track, const FGuid& BindingGuid)
		{
			TSharedRef<FJsonObject> TrackJson = MakeShared<FJsonObject>();
			TrackJson->SetStringField(TEXT("name"), Track ? Track->GetName() : FString());
			TrackJson->SetStringField(TEXT("class"), (Track && Track->GetClass()) ? Track->GetClass()->GetPathName() : FString());
			TrackJson->SetStringField(TEXT("display_name"), Track ? Track->GetDisplayName().ToString() : FString());
			TrackJson->SetStringField(TEXT("binding_guid"), BindingGuid.IsValid() ? BindingGuid.ToString() : FString());
			TrackJson->SetStringField(TEXT("binding_scope"), BindingGuid.IsValid() ? TEXT("widget") : TEXT("master"));
			if (const FWidgetAnimationBinding* AnimationBinding = FindUmgAnimationBindingByGuid(Animation, BindingGuid))
			{
				TrackJson->SetStringField(TEXT("widget_name"), AnimationBinding->WidgetName.ToString());
				TrackJson->SetStringField(TEXT("slot_widget_name"), AnimationBinding->SlotWidgetName.ToString());
				TrackJson->SetBoolField(TEXT("is_root_widget"), AnimationBinding->bIsRootWidget);
			}
			if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
			{
				TrackJson->SetStringField(TEXT("property_name"), PropertyTrack->GetPropertyName().ToString());
				TrackJson->SetStringField(TEXT("property_path"), PropertyTrack->GetPropertyPath().ToString());
			}
			const int32 KeyframeCount = CountMovieSceneTrackKeyframes(Track);
			TrackJson->SetNumberField(TEXT("section_count"), Track ? Track->GetAllSections().Num() : 0);
			TrackJson->SetNumberField(TEXT("keyframe_count"), KeyframeCount);
			TArray<TSharedPtr<FJsonValue>> SectionsJson;
			if (Track)
			{
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					if (!Section)
					{
						continue;
					}
					TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
					SectionJson->SetStringField(TEXT("name"), Section->GetName());
					SectionJson->SetStringField(TEXT("class"), Section->GetClass() ? Section->GetClass()->GetPathName() : FString());
					SectionJson->SetBoolField(TEXT("is_active"), Section->IsActive());
					SectionJson->SetBoolField(TEXT("is_locked"), Section->IsLocked());
					SectionJson->SetNumberField(TEXT("keyframe_count"), CountMovieSceneSectionKeyframes(Section));
					SectionsJson.Add(MakeShared<FJsonValueObject>(SectionJson));
				}
			}
			TrackJson->SetArrayField(TEXT("sections"), SectionsJson);
			return TrackJson;
		}

		void SetUmgAnimationReceiptBase(
			TSharedRef<FJsonObject>& OutStructured,
			const FString& ToolName,
			const FString& AssetPath,
			const FString& AnimationName,
			const FString& WidgetName,
			const FString& TrackType,
			UWidgetAnimation* Animation,
			bool bReceiptComplete)
		{
			int32 TrackCount = 0;
			int32 KeyframeCount = 0;
			CountUmgAnimationTracksAndKeyframes(Animation, TrackCount, KeyframeCount);
			OutStructured->SetStringField(TEXT("tool"), ToolName);
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("animation"), AnimationName);
			OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
			OutStructured->SetStringField(TEXT("track_type"), TrackType);
			OutStructured->SetNumberField(TEXT("track_count"), TrackCount);
			OutStructured->SetNumberField(TEXT("keyframe_count"), KeyframeCount);
			OutStructured->SetBoolField(TEXT("receipt_complete"), bReceiptComplete);
			OutStructured->SetStringField(TEXT("compile_or_refresh_hint"), GetUmgAnimationCompileOrRefreshHint());
		}

		void SetUmgAnimationFailClosedReceipt(
			TSharedRef<FJsonObject>& OutStructured,
			const FString& ToolName,
			const FString& AssetPath,
			const FString& AnimationName,
			const FString& WidgetName,
			const FString& TrackType,
			UWidgetAnimation* Animation,
			const FString& Error)
		{
			SetUmgAnimationReceiptBase(OutStructured, ToolName, AssetPath, AnimationName, WidgetName, TrackType, Animation, false);
			OutStructured->SetStringField(TEXT("status"), TEXT("fail_closed"));
			OutStructured->SetBoolField(TEXT("safe_fail_closed"), true);
			OutStructured->SetStringField(TEXT("error"), Error);
		}

		bool NormalizeUmgAnimationProperty(UWidget* Widget, EUmgAnimationTrackKind Kind, FString& PropertyName, FString& PropertyPath, FString& OutError)
		{
			if (!Widget)
			{
				OutError = TEXT("Widget was not found in widget tree.");
				return false;
			}
			if (Kind == EUmgAnimationTrackKind::Float)
			{
				if (PropertyName.IsEmpty() && PropertyPath.IsEmpty())
				{
					PropertyName = TEXT("RenderOpacity");
					PropertyPath = TEXT("RenderOpacity");
				}
				else if (PropertyPath.IsEmpty())
				{
					PropertyPath = PropertyName;
				}
				else if (PropertyName.IsEmpty())
				{
					PropertyName = PropertyPath;
				}
				if (PropertyPath.Contains(TEXT(".")))
				{
					OutError = FString::Printf(TEXT("Nested float property path '%s' is not supported by this safe UMG animation tool."), *PropertyPath);
					return false;
				}
				FProperty* Property = Widget->GetClass()->FindPropertyByName(FName(*PropertyName));
				FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
				if (!NumericProperty || !NumericProperty->IsFloatingPoint())
				{
					OutError = FString::Printf(TEXT("Property '%s' is not a direct floating-point property on widget '%s'."), *PropertyName, *Widget->GetName());
					return false;
				}
				return true;
			}
			if (Kind == EUmgAnimationTrackKind::Transform2D)
			{
				if (PropertyName.IsEmpty() && PropertyPath.IsEmpty())
				{
					PropertyName = TEXT("RenderTransform");
					PropertyPath = TEXT("RenderTransform");
				}
				else if (PropertyPath.IsEmpty())
				{
					PropertyPath = PropertyName;
				}
				else if (PropertyName.IsEmpty())
				{
					PropertyName = PropertyPath;
				}
				if (PropertyPath.Contains(TEXT(".")))
				{
					OutError = FString::Printf(TEXT("Nested 2D transform property path '%s' is not supported; use property_path='RenderTransform' plus a channel."), *PropertyPath);
					return false;
				}
				FProperty* Property = Widget->GetClass()->FindPropertyByName(FName(*PropertyName));
				FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				if (!StructProperty || StructProperty->Struct != FWidgetTransform::StaticStruct())
				{
					OutError = FString::Printf(TEXT("Property '%s' is not a direct FWidgetTransform property on widget '%s'."), *PropertyName, *Widget->GetName());
					return false;
				}
				return true;
			}
			OutError = TEXT("Unsupported UMG animation track_type. Supported values: float, render_opacity, 2d_transform, render_transform.");
			return false;
		}

		bool ResolveUmgAnimationBinding(UWidgetBlueprint* WidgetBlueprint, UWidgetAnimation* Animation, UWidget* Widget, const FString& WidgetName, FGuid& OutGuid, FString& OutError)
		{
			UMovieScene* MovieScene = Animation ? Animation->MovieScene : nullptr;
			if (!WidgetBlueprint || !Animation || !MovieScene || !Widget)
			{
				OutError = TEXT("Invalid widget blueprint, animation, movie scene, or widget.");
				return false;
			}
			for (FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
			{
				if (Binding.WidgetName == FName(*WidgetName) && Binding.SlotWidgetName == NAME_None && !Binding.bIsRootWidget)
				{
					if (Binding.AnimationGuid.IsValid() && MovieScene->FindBinding(Binding.AnimationGuid))
					{
						OutGuid = Binding.AnimationGuid;
						return true;
					}
					const FGuid NewGuid = MovieScene->AddPossessable(WidgetName, Widget->GetClass());
					if (!NewGuid.IsValid() || !MovieScene->FindBinding(NewGuid))
					{
						OutError = TEXT("Failed to repair movie scene binding for widget animation.");
						return false;
					}
					Binding.AnimationGuid = NewGuid;
					OutGuid = NewGuid;
					return true;
				}
			}
			const FGuid NewGuid = MovieScene->AddPossessable(WidgetName, Widget->GetClass());
			if (!NewGuid.IsValid() || !MovieScene->FindBinding(NewGuid))
			{
				OutError = TEXT("Failed to create movie scene possessable binding for widget.");
				return false;
			}
			FWidgetAnimationBinding NewBinding;
			NewBinding.WidgetName = FName(*WidgetName);
			NewBinding.SlotWidgetName = NAME_None;
			NewBinding.AnimationGuid = NewGuid;
			NewBinding.bIsRootWidget = false;
			Animation->AnimationBindings.Add(NewBinding);
			OutGuid = NewGuid;
			return true;
		}

		UMovieScenePropertyTrack* FindUmgAnimationPropertyTrack(UMovieScene* MovieScene, const FGuid& BindingGuid, TSubclassOf<UMovieSceneTrack> TrackClass, const FString& PropertyPath)
		{
			if (!MovieScene || !BindingGuid.IsValid())
			{
				return nullptr;
			}
			for (UMovieSceneTrack* Track : MovieScene->FindTracks(TrackClass, BindingGuid))
			{
				UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
				if (PropertyTrack && PropertyTrack->GetPropertyPath() == FName(*PropertyPath))
				{
					return PropertyTrack;
				}
			}
			return nullptr;
		}

		UMovieScenePropertyTrack* EnsureUmgAnimationPropertyTrack(
			UMovieScene* MovieScene,
			const FGuid& BindingGuid,
			EUmgAnimationTrackKind Kind,
			const FString& PropertyName,
			const FString& PropertyPath,
			bool& bOutCreated,
			FString& OutError)
		{
			bOutCreated = false;
			if (!MovieScene || !BindingGuid.IsValid())
			{
				OutError = TEXT("MovieScene or binding guid is invalid.");
				return nullptr;
			}
			TSubclassOf<UMovieSceneTrack> TrackClass = nullptr;
			if (Kind == EUmgAnimationTrackKind::Float)
			{
				TrackClass = UMovieSceneFloatTrack::StaticClass();
			}
			else if (Kind == EUmgAnimationTrackKind::Transform2D)
			{
				TrackClass = UMovieScene2DTransformTrack::StaticClass();
			}
			if (!TrackClass)
			{
				OutError = TEXT("Unsupported UMG animation track type.");
				return nullptr;
			}
			if (UMovieScenePropertyTrack* ExistingTrack = FindUmgAnimationPropertyTrack(MovieScene, BindingGuid, TrackClass, PropertyPath))
			{
				return ExistingTrack;
			}
			UMovieScenePropertyTrack* NewTrack = Cast<UMovieScenePropertyTrack>(MovieScene->AddTrack(TrackClass, BindingGuid));
			if (!NewTrack)
			{
				OutError = FString::Printf(TEXT("Failed to add track class '%s'. A track of the same class may already exist for another property."), *TrackClass->GetPathName());
				return nullptr;
			}
			NewTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);
#if WITH_EDITORONLY_DATA
			NewTrack->SetDisplayName(FText::FromString(PropertyPath));
#endif
			bOutCreated = true;
			return NewTrack;
		}

		UMovieSceneSection* EnsureUmgAnimationSection(UMovieScenePropertyTrack* Track, FFrameNumber Frame, bool& bOutCreated, FString& OutError)
		{
			bOutCreated = false;
			if (!Track)
			{
				OutError = TEXT("Animation property track is null.");
				return nullptr;
			}
			UMovieSceneSection* Section = Track->FindOrAddSection(Frame, bOutCreated);
			if (!Section)
			{
				OutError = TEXT("Failed to find or create animation section.");
				return nullptr;
			}
			if (bOutCreated)
			{
				Section->SetRange(TRange<FFrameNumber>::All());
			}
			return Section;
		}

		bool Resolve2DTransformChannel(UMovieScene2DTransformSection* Section, const FString& ChannelName, FMovieSceneFloatChannel*& OutChannel, EMovieScene2DTransformChannel& OutMask, FString& OutError)
		{
			OutChannel = nullptr;
			OutMask = EMovieScene2DTransformChannel::None;
			if (!Section)
			{
				OutError = TEXT("2D transform section is null.");
				return false;
			}
			const FString Normalized = NormalizeUmgAnimationToken(ChannelName);
			if (Normalized == TEXT("translation_x") || Normalized == TEXT("x"))
			{
				OutChannel = &Section->Translation[0];
				OutMask = EMovieScene2DTransformChannel::TranslationX;
			}
			else if (Normalized == TEXT("translation_y") || Normalized == TEXT("y"))
			{
				OutChannel = &Section->Translation[1];
				OutMask = EMovieScene2DTransformChannel::TranslationY;
			}
			else if (Normalized == TEXT("rotation") || Normalized == TEXT("angle"))
			{
				OutChannel = &Section->Rotation;
				OutMask = EMovieScene2DTransformChannel::Rotation;
			}
			else if (Normalized == TEXT("scale_x"))
			{
				OutChannel = &Section->Scale[0];
				OutMask = EMovieScene2DTransformChannel::ScaleX;
			}
			else if (Normalized == TEXT("scale_y"))
			{
				OutChannel = &Section->Scale[1];
				OutMask = EMovieScene2DTransformChannel::ScaleY;
			}
			else if (Normalized == TEXT("shear_x"))
			{
				OutChannel = &Section->Shear[0];
				OutMask = EMovieScene2DTransformChannel::ShearX;
			}
			else if (Normalized == TEXT("shear_y"))
			{
				OutChannel = &Section->Shear[1];
				OutMask = EMovieScene2DTransformChannel::ShearY;
			}
			if (!OutChannel)
			{
				OutError = TEXT("Unsupported 2D transform channel. Use translation_x, translation_y, rotation, scale_x, scale_y, shear_x, or shear_y.");
				return false;
			}
			return true;
		}

		int32 AddUmgFloatKey(FMovieSceneFloatChannel& Channel, FFrameNumber Frame, float Value, const FString& Interpolation)
		{
			const FString Normalized = NormalizeUmgAnimationToken(Interpolation.IsEmpty() ? TEXT("linear") : Interpolation);
			if (Normalized == TEXT("constant"))
			{
				return Channel.AddConstantKey(Frame, Value);
			}
			if (Normalized == TEXT("cubic") || Normalized == TEXT("auto"))
			{
				return Channel.AddCubicKey(Frame, Value);
			}
			return Channel.AddLinearKey(Frame, Value);
		}

		bool ReadBackUmgFloatKey(const FMovieSceneFloatChannel& Channel, FFrameNumber Frame, float ExpectedValue, float& OutValue)
		{
			const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
			const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
			for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
			{
				if (Times[Index] == Frame)
				{
					OutValue = Values[Index].Value;
					if (FMath::IsNearlyEqual(OutValue, ExpectedValue, 0.0001f))
					{
						return true;
					}
				}
			}
			return false;
		}

		UClass* ResolveMovieSceneTrackClass(const FString& TrackClassPath, FString& OutError)
		{
			if (TrackClassPath.IsEmpty())
			{
				OutError = TEXT("track_class_path is required.");
				return nullptr;
			}
			UClass* TrackClass = StaticLoadClass(UMovieSceneTrack::StaticClass(), nullptr, *TrackClassPath);
			if (!TrackClass && !TrackClassPath.StartsWith(TEXT("/Script/")))
			{
				FString ClassName = TrackClassPath;
				ClassName.RemoveFromStart(TEXT("U"));
				TrackClass = StaticLoadClass(
					UMovieSceneTrack::StaticClass(),
					nullptr,
					*FString::Printf(TEXT("/Script/MovieSceneTracks.%s"), *ClassName));
			}
			if (!TrackClass || !TrackClass->IsChildOf(UMovieSceneTrack::StaticClass()))
			{
				OutError = FString::Printf(TEXT("Track class is unavailable or is not a UMovieSceneTrack: %s"), *TrackClassPath);
				return nullptr;
			}
			if (TrackClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				OutError = FString::Printf(TEXT("Track class cannot be instantiated safely: %s"), *TrackClass->GetPathName());
				return nullptr;
			}
			return TrackClass;
		}

		bool ResolveMovieSceneBindingGuid(UMovieScene* MovieScene, const FString& BindingId, FGuid& OutGuid, FString& OutError)
		{
			OutGuid.Invalidate();
			if (BindingId.IsEmpty())
			{
				return true;
			}
			if (!FGuid::Parse(BindingId, OutGuid) || !OutGuid.IsValid())
			{
				OutError = FString::Printf(TEXT("binding_id is not a valid GUID: %s"), *BindingId);
				return false;
			}
			if (!MovieScene || !MovieScene->FindBinding(OutGuid))
			{
				OutError = FString::Printf(TEXT("MovieScene binding was not found: %s"), *BindingId);
				return false;
			}
			return true;
		}

		FGuid FindSequencePossessableBindingByActorName(UMovieScene* MovieScene, const AActor* Actor)
		{
			if (!MovieScene || !Actor)
			{
				return FGuid();
			}
			const FString ActorLabel = Actor->GetActorLabel();
			const FString ActorName = Actor->GetName();
			const UMovieScene* ConstMovieScene = MovieScene;
			for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
			{
				if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Binding.GetObjectGuid()))
				{
					if (Possessable->GetName().Equals(ActorLabel, ESearchCase::IgnoreCase)
						|| Possessable->GetName().Equals(ActorName, ESearchCase::IgnoreCase))
					{
						return Binding.GetObjectGuid();
					}
				}
			}
			return FGuid();
		}

		UMovieSceneTrack* FindMovieSceneTrackByName(UMovieScene* MovieScene, const FGuid& BindingGuid, const FString& TrackName)
		{
			if (!MovieScene)
			{
				return nullptr;
			}
			const TArray<UMovieSceneTrack*>* Tracks = nullptr;
			TArray<UMovieSceneTrack*> MasterTracks;
			if (BindingGuid.IsValid())
			{
				const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
				if (!Binding)
				{
					return nullptr;
				}
				Tracks = &Binding->GetTracks();
			}
			else
			{
				MasterTracks = MovieScene->GetTracks();
				Tracks = &MasterTracks;
			}
			for (UMovieSceneTrack* Track : *Tracks)
			{
				if (Track && (Track->GetTrackName() == FName(*TrackName)
					|| Track->GetName().Equals(TrackName, ESearchCase::IgnoreCase)
					|| Track->GetDisplayName().ToString().Equals(TrackName, ESearchCase::IgnoreCase)))
				{
					return Track;
				}
			}
			return nullptr;
		}

		struct FResolvedSequenceChannel
		{
			ULevelSequence* Sequence = nullptr;
			UMovieScene* MovieScene = nullptr;
			UMovieSceneSection* Section = nullptr;
			FMovieSceneChannel* Channel = nullptr;
			FName ChannelType;
			FString ChannelName;
			int32 ChannelTypeIndex = INDEX_NONE;
			int32 AbsoluteChannelIndex = INDEX_NONE;
		};

		bool SequenceChannelTokenMatches(const FString& Requested, const FString& Candidate)
		{
			if (Requested.IsEmpty())
			{
				return true;
			}
			FString NormalizedRequested = Requested;
			FString NormalizedCandidate = Candidate;
			NormalizedRequested.ReplaceInline(TEXT("_"), TEXT(""));
			NormalizedCandidate.ReplaceInline(TEXT("_"), TEXT(""));
			NormalizedRequested.RemoveFromStart(TEXT("F"));
			NormalizedCandidate.RemoveFromStart(TEXT("F"));
			return NormalizedRequested.Equals(NormalizedCandidate, ESearchCase::IgnoreCase)
				|| NormalizedCandidate.Contains(NormalizedRequested, ESearchCase::IgnoreCase);
		}

		bool ResolveSequenceChannel(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			FResolvedSequenceChannel& OutResolved,
			FString& OutError)
		{
			FString AssetPath;
			FString TrackName;
			FString BindingId;
			FString RequestedChannelName;
			FString RequestedChannelType;
			int32 SectionIndex = 0;
			int32 RequestedChannelIndex = 0;
			int32 RequestedAbsoluteIndex = INDEX_NONE;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)
				|| !Arguments->TryGetStringField(TEXT("track_name"), TrackName)
				|| !Arguments->TryGetNumberField(TEXT("section_index"), SectionIndex))
			{
				OutError = TEXT("asset_path, track_name, and section_index are required.");
				return false;
			}
			Arguments->TryGetStringField(TEXT("binding_id"), BindingId);
			Arguments->TryGetStringField(TEXT("channel_name"), RequestedChannelName);
			Arguments->TryGetStringField(TEXT("channel_type"), RequestedChannelType);
			Arguments->TryGetNumberField(TEXT("channel_index"), RequestedChannelIndex);
			Arguments->TryGetNumberField(TEXT("absolute_channel_index"), RequestedAbsoluteIndex);
			if (RequestedChannelName.IsEmpty() && RequestedChannelType.IsEmpty() && RequestedAbsoluteIndex == INDEX_NONE)
			{
				OutError = TEXT("Provide channel_name, channel_type, or absolute_channel_index.");
				return false;
			}

			OutResolved.Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!OutResolved.Sequence)
			{
				return false;
			}
			OutResolved.MovieScene = OutResolved.Sequence->GetMovieScene();
			if (!OutResolved.MovieScene)
			{
				OutError = TEXT("Level sequence has no MovieScene.");
				return false;
			}
			FGuid BindingGuid;
			if (!ResolveMovieSceneBindingGuid(OutResolved.MovieScene, BindingId, BindingGuid, OutError))
			{
				return false;
			}
			UMovieSceneTrack* Track = FindMovieSceneTrackByName(OutResolved.MovieScene, BindingGuid, TrackName);
			if (!Track)
			{
				OutError = FString::Printf(TEXT("MovieScene track was not found: %s"), *TrackName);
				return false;
			}
			const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
			if (SectionIndex < 0 || SectionIndex >= Sections.Num() || !Sections[SectionIndex])
			{
				OutError = FString::Printf(TEXT("section_index %d is out of range for track '%s'."), SectionIndex, *TrackName);
				return false;
			}
			OutResolved.Section = Sections[SectionIndex];

			int32 AbsoluteIndex = 0;
			for (const FMovieSceneChannelEntry& Entry : OutResolved.Section->GetChannelProxy().GetAllEntries())
			{
				const FString TypeName = Entry.GetChannelTypeName().ToString();
				const TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
#if WITH_EDITOR
				const TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
#endif
				for (int32 TypeIndex = 0; TypeIndex < Channels.Num(); ++TypeIndex, ++AbsoluteIndex)
				{
					FString MetaName;
#if WITH_EDITOR
					if (TypeIndex >= 0 && TypeIndex < MetaData.Num())
					{
						MetaName = MetaData[TypeIndex].Name.ToString();
						if (MetaName.IsEmpty())
						{
							MetaName = MetaData[TypeIndex].DisplayText.ToString();
						}
					}
#endif
					const bool bAbsoluteMatch = RequestedAbsoluteIndex != INDEX_NONE && RequestedAbsoluteIndex == AbsoluteIndex;
					const bool bTypeMatch = RequestedAbsoluteIndex == INDEX_NONE
						&& SequenceChannelTokenMatches(RequestedChannelType, TypeName)
						&& TypeIndex == RequestedChannelIndex;
					const bool bNameMatch = RequestedAbsoluteIndex == INDEX_NONE
						&& !RequestedChannelName.IsEmpty()
						&& (SequenceChannelTokenMatches(RequestedChannelName, MetaName)
							|| (Channels.Num() == 1 && SequenceChannelTokenMatches(RequestedChannelName, TypeName)));
					if ((bAbsoluteMatch || bTypeMatch || bNameMatch) && Channels[TypeIndex])
					{
						OutResolved.Channel = Channels[TypeIndex];
						OutResolved.ChannelType = Entry.GetChannelTypeName();
						OutResolved.ChannelName = MetaName.IsEmpty() ? FString::Printf(TEXT("%s[%d]"), *TypeName, TypeIndex) : MetaName;
						OutResolved.ChannelTypeIndex = TypeIndex;
						OutResolved.AbsoluteChannelIndex = AbsoluteIndex;
						return true;
					}
				}
			}
			OutError = FString::Printf(
				TEXT("Sequence channel was not found (name='%s', type='%s', type_index=%d, absolute_index=%d)."),
				*RequestedChannelName,
				*RequestedChannelType,
				RequestedChannelIndex,
				RequestedAbsoluteIndex);
			return false;
		}

		bool SequenceJsonValueToDouble(const TSharedPtr<FJsonValue>& JsonValue, double& OutValue)
		{
			if (!JsonValue.IsValid())
			{
				return false;
			}
			if (JsonValue->Type == EJson::Number)
			{
				OutValue = JsonValue->AsNumber();
				return true;
			}
			if (JsonValue->Type == EJson::String)
			{
				return LexTryParseString(OutValue, *JsonValue->AsString());
			}
			return false;
		}

		bool SequenceJsonValueToBool(const TSharedPtr<FJsonValue>& JsonValue, bool& OutValue)
		{
			if (!JsonValue.IsValid())
			{
				return false;
			}
			if (JsonValue->Type == EJson::Boolean)
			{
				OutValue = JsonValue->AsBool();
				return true;
			}
			if (JsonValue->Type == EJson::Number)
			{
				OutValue = !FMath::IsNearlyZero(JsonValue->AsNumber());
				return true;
			}
			if (JsonValue->Type == EJson::String)
			{
				const FString Value = JsonValue->AsString();
				OutValue = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
					|| Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
					|| Value == TEXT("1");
				return true;
			}
			return false;
		}

		bool HasDoubleChannelKey(const FMovieSceneDoubleChannel& Channel, FFrameNumber Frame, double ExpectedValue)
		{
			const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
			const TArrayView<const FMovieSceneDoubleValue> Values = Channel.GetValues();
			for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
			{
				if (Times[Index] == Frame && FMath::IsNearlyEqual(Values[Index].Value, ExpectedValue, 0.0001))
				{
					return true;
				}
			}
			return false;
		}

		TSharedRef<FJsonObject> AssetDataToJson(const FAssetData& Asset)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			Json->SetStringField(TEXT("objectPath"), Asset.GetObjectPathString());
			Json->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
			Json->SetStringField(TEXT("packagePath"), Asset.PackagePath.ToString());
			Json->SetStringField(TEXT("classPath"), Asset.AssetClassPath.ToString());
			return Json;
		}

		bool VerifyCreatedAssetReloaded(FSololmcpEditorServices& Services, UObject* Asset, UClass* ExpectedClass, TSharedRef<FJsonObject>& OutStructured, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("Asset creation returned null.");
				return false;
			}
			const FString CreatedPath = Asset->GetPathName();
			if (!Services.AssetExists(CreatedPath))
			{
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("error"), TEXT("asset_not_persisted_after_create"));
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				OutError = FString::Printf(TEXT("asset_not_persisted_after_create: %s"), *CreatedPath);
				return false;
			}
			FString ReloadError;
			UObject* ReloadedAsset = Services.LoadAsset(CreatedPath, ReloadError);
			if (!ReloadedAsset)
			{
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("error"), TEXT("asset_reload_failed_after_create"));
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				if (!ReloadError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("reload_error"), ReloadError);
				}
				OutError = FString::Printf(TEXT("asset_reload_failed_after_create: %s"), *CreatedPath);
				return false;
			}
			if (ExpectedClass && !ReloadedAsset->IsA(ExpectedClass))
			{
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("error"), TEXT("asset_class_mismatch_after_create"));
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				OutStructured->SetStringField(TEXT("expected_class"), ExpectedClass->GetPathName());
				OutStructured->SetStringField(TEXT("actual_class"), ReloadedAsset->GetClass()->GetPathName());
				OutError = FString::Printf(TEXT("asset_class_mismatch_after_create: expected %s, got %s at %s"),
					*ExpectedClass->GetPathName(), *ReloadedAsset->GetClass()->GetPathName(), *CreatedPath);
				return false;
			}
			OutStructured->SetBoolField(TEXT("reload_verified"), true);
			return true;
		}

		TSharedRef<FJsonObject> WorldStateToJson(FSololmcpEditorServices& Services)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			FString Error;
			UWorld* World = Services.GetEditorWorld(Error);
			Result->SetStringField(TEXT("world"), World ? World->GetPathName() : FString());

			if (GEditor)
			{
				TArray<TSharedPtr<FJsonValue>> Selected;
				USelection* Selection = GEditor->GetSelectedActors();
				if (Selection)
				{
					for (FSelectionIterator It(*Selection); It; ++It)
					{
						if (const AActor* Actor = Cast<AActor>(*It))
						{
							Selected.Add(MakeShared<FJsonValueObject>(FSololmcpEditorServices::MakeActorReference(Actor)));
						}
					}
				}
				Result->SetArrayField(TEXT("selectedActors"), Selected);
			}

			if (ULevelEditorSubsystem* LevelSubsystem = Services.GetLevelEditorSubsystem(Error))
			{
				if (ULevel* CurrentLevel = LevelSubsystem->GetCurrentLevel())
				{
					Result->SetStringField(TEXT("currentLevel"), CurrentLevel->GetPathName());
				}
			}

			return Result;
		}

		bool ResolveTransformArg(const TSharedRef<FJsonObject>& Arguments, FTransform& OutTransform)
		{
			TSharedPtr<FJsonObject> TransformObject;
			if (!TryGetObjectField(Arguments, TEXT("transform"), TransformObject))
			{
				return false;
			}
			return FSololmcpEditorServices::JsonToTransform(TransformObject, OutTransform);
		}

		FString NormalizeLevelPackagePathForCheck(const FString& AssetPath)
		{
			FString PackagePath = AssetPath.TrimStartAndEnd();
			int32 DotIndex = INDEX_NONE;
			if (PackagePath.FindChar(TCHAR('.'), DotIndex))
			{
				PackagePath = PackagePath.Left(DotIndex);
			}
			return PackagePath;
		}

		bool VerifyCurrentLevelPackage(ULevelEditorSubsystem* LevelSubsystem, const FString& RequestedAssetPath, FString& OutError)
		{
			ULevel* CurrentLevel = LevelSubsystem ? LevelSubsystem->GetCurrentLevel() : nullptr;
			if (!CurrentLevel || !CurrentLevel->GetOutermost())
			{
				OutError = TEXT("Level operation reported success but no current level is loaded.");
				return false;
			}

			const FString ExpectedPackagePath = NormalizeLevelPackagePathForCheck(RequestedAssetPath);
			const FString ActualPackagePath = CurrentLevel->GetOutermost()->GetName();
			if (!ExpectedPackagePath.IsEmpty() && !ActualPackagePath.Equals(ExpectedPackagePath, ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("Level operation reported success but current level is '%s' instead of '%s'."), *ActualPackagePath, *ExpectedPackagePath);
				return false;
			}
			return true;
		}

		bool IsDataViewWidgetClassName(const FString& ClassName)
		{
			return ClassName.Contains(TEXT("ListView")) || ClassName.Contains(TEXT("TileView")) || ClassName.Contains(TEXT("TreeView"));
		}

		TSharedRef<FJsonObject> MakeUmgDataViewContractReceipt(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget, const FString& WidgetName, const FString& ToolName)
		{
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(TEXT("status"), TEXT("contract_ready"));
			Receipt->SetStringField(TEXT("execution_mode"), TEXT("contract_only"));
			Receipt->SetBoolField(TEXT("mutated_asset"), false);
			Receipt->SetStringField(TEXT("tool"), ToolName);
			Receipt->SetStringField(TEXT("asset_path"), WidgetBlueprint ? WidgetBlueprint->GetPathName() : FString());
			Receipt->SetStringField(TEXT("widget_name"), WidgetName);
			Receipt->SetStringField(TEXT("widget_class"), Widget ? Widget->GetClass()->GetName() : FString());
			Receipt->SetStringField(TEXT("contract"), TEXT("validated_widget_and_recorded_binding_intent; native Blueprint graph mutation is deferred until the UE API path is hardened"));
			return Receipt;
		}

		bool VerifyNumericPropertyApplied(UObject* Object, const TCHAR* PropertyName, const double Expected, const double Tolerance, FString& OutError)
		{
			if (!Object)
			{
				OutError = TEXT("Cannot verify property on a null object.");
				return false;
			}
			FProperty* Property = Object->GetClass()->FindPropertyByName(FName(PropertyName));
			FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
			if (!NumericProperty)
			{
				OutError = FString::Printf(TEXT("Property '%s' was requested but is not numeric on %s."), PropertyName, *Object->GetClass()->GetName());
				return false;
			}
			const void* ValuePtr = NumericProperty->ContainerPtrToValuePtr<void>(Object);
			const double Actual = NumericProperty->IsFloatingPoint()
				? NumericProperty->GetFloatingPointPropertyValue(ValuePtr)
				: static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr));
			if (!FMath::IsNearlyEqual(Actual, Expected, Tolerance))
			{
				OutError = FString::Printf(TEXT("Property '%s' did not apply: requested %.6f, actual %.6f."), PropertyName, Expected, Actual);
				return false;
			}
			return true;
		}

		bool VerifyBoolPropertyApplied(UObject* Object, const TCHAR* PropertyName, const bool bExpected, FString& OutError)
		{
			if (!Object)
			{
				OutError = TEXT("Cannot verify property on a null object.");
				return false;
			}
			FProperty* Property = Object->GetClass()->FindPropertyByName(FName(PropertyName));
			FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property);
			if (!BoolProperty)
			{
				OutError = FString::Printf(TEXT("Property '%s' was requested but is not boolean on %s."), PropertyName, *Object->GetClass()->GetName());
				return false;
			}
			const bool bActual = BoolProperty->GetPropertyValue_InContainer(Object);
			if (bActual != bExpected)
			{
				OutError = FString::Printf(TEXT("Property '%s' did not apply: requested %s, actual %s."), PropertyName, bExpected ? TEXT("true") : TEXT("false"), bActual ? TEXT("true") : TEXT("false"));
				return false;
			}
			return true;
		}

		bool VerifyObjectPropertyApplied(UObject* Object, const TCHAR* PropertyName, UObject* Expected, FString& OutError)
		{
			if (!Object)
			{
				OutError = TEXT("Cannot verify property on a null object.");
				return false;
			}
			FProperty* Property = Object->GetClass()->FindPropertyByName(FName(PropertyName));
			FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
			if (!ObjectProperty)
			{
				OutError = FString::Printf(TEXT("Property '%s' was requested but is not an object property on %s."), PropertyName, *Object->GetClass()->GetName());
				return false;
			}
			UObject* Actual = ObjectProperty->GetObjectPropertyValue_InContainer(Object);
			if (Actual != Expected)
			{
				OutError = FString::Printf(TEXT("Property '%s' did not apply: requested %s, actual %s."),
					PropertyName,
					Expected ? *Expected->GetPathName() : TEXT("None"),
					Actual ? *Actual->GetPathName() : TEXT("None"));
				return false;
			}
			return true;
		}

		UClass* ResolveActorClassByType(FSololmcpEditorServices& Services, const FString& Type, FString& OutError)
		{
			if (Type == TEXT("directional"))
			{
				return ADirectionalLight::StaticClass();
			}
			if (Type == TEXT("point"))
			{
				return APointLight::StaticClass();
			}
			if (Type == TEXT("spot"))
			{
				return ASpotLight::StaticClass();
			}
			if (Type == TEXT("rect"))
			{
				return ARectLight::StaticClass();
			}
			if (Type == TEXT("sky"))
			{
				return ASkyLight::StaticClass();
			}
			if (Type == TEXT("camera"))
			{
				return ACameraActor::StaticClass();
			}
			if (Type == TEXT("cine"))
			{
				return ACineCameraActor::StaticClass();
			}
			return Services.ResolveClass(Type, OutError);
		}

		bool TrySplitGenericInner(const FString& TypeName, const FString& Prefix, FString& OutInner)
		{
			if (!TypeName.StartsWith(Prefix) || !TypeName.EndsWith(TEXT(">")))
			{
				return false;
			}
			OutInner = TypeName.Mid(Prefix.Len(), TypeName.Len() - Prefix.Len() - 1).TrimStartAndEnd();
			return !OutInner.IsEmpty();
		}

		bool TrySplitMapTypes(const FString& TypeName, FString& OutKeyType, FString& OutValueType)
		{
			FString Inner;
			if (!TrySplitGenericInner(TypeName, TEXT("map<"), Inner))
			{
				return false;
			}

			int32 Depth = 0;
			for (int32 Index = 0; Index < Inner.Len(); ++Index)
			{
				const TCHAR Char = Inner[Index];
				if (Char == TCHAR('<')) { ++Depth; }
				else if (Char == TCHAR('>')) { --Depth; }
				else if (Char == TCHAR(',') && Depth == 0)
				{
					OutKeyType = Inner.Left(Index).TrimStartAndEnd();
					OutValueType = Inner.Mid(Index + 1).TrimStartAndEnd();
					return !OutKeyType.IsEmpty() && !OutValueType.IsEmpty();
				}
			}

			return false;
		}

		bool MakeBlueprintPinType(const FString& TypeName, FEdGraphPinType& OutPinType)
		{
			const FString Normalized = TypeName.TrimStartAndEnd();
			OutPinType.ResetToDefaults();

			FString InnerType;
			if (TrySplitGenericInner(Normalized, TEXT("array<"), InnerType))
			{
				if (!MakeBlueprintPinType(InnerType, OutPinType))
				{
					return false;
				}
				OutPinType.ContainerType = EPinContainerType::Array;
				return true;
			}
			if (TrySplitGenericInner(Normalized, TEXT("set<"), InnerType))
			{
				if (!MakeBlueprintPinType(InnerType, OutPinType))
				{
					return false;
				}
				OutPinType.ContainerType = EPinContainerType::Set;
				return true;
			}

			FString KeyTypeName;
			FString ValueTypeName;
			if (TrySplitMapTypes(Normalized, KeyTypeName, ValueTypeName))
			{
				FEdGraphPinType KeyType;
				FEdGraphPinType ValueType;
				if (!MakeBlueprintPinType(KeyTypeName, KeyType) || !MakeBlueprintPinType(ValueTypeName, ValueType))
				{
					return false;
				}

				OutPinType = KeyType;
				OutPinType.ContainerType = EPinContainerType::Map;
				OutPinType.PinValueType.TerminalCategory = ValueType.PinCategory;
				OutPinType.PinValueType.TerminalSubCategory = ValueType.PinSubCategory;
				OutPinType.PinValueType.TerminalSubCategoryObject = ValueType.PinSubCategoryObject;
				OutPinType.PinValueType.bTerminalIsConst = ValueType.bIsConst;
				OutPinType.PinValueType.bTerminalIsWeakPointer = ValueType.bIsWeakPointer;
				return true;
			}

			if (Normalized == TEXT("bool"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
				return true;
			}
			if (Normalized == TEXT("int"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
				return true;
			}
			if (Normalized == TEXT("int64"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
				return true;
			}
			if (Normalized == TEXT("byte"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				return true;
			}
			if (Normalized == TEXT("float"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
				OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
				return true;
			}
			if (Normalized == TEXT("double"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
				OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
				return true;
			}
			if (Normalized == TEXT("string"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
				return true;
			}
			if (Normalized == TEXT("name"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
				return true;
			}
			if (Normalized == TEXT("text"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
				return true;
			}
			if (Normalized == TEXT("vector"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
				return true;
			}
			if (Normalized == TEXT("vector2d"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
				return true;
			}
			if (Normalized == TEXT("color"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
				return true;
			}
			if (Normalized == TEXT("rotator"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
				return true;
			}
			if (Normalized == TEXT("transform"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
				return true;
			}

			if (Normalized.StartsWith(TEXT("object<")) && TrySplitGenericInner(Normalized, TEXT("object<"), InnerType))
			{
				UClass* ObjectClass = LoadObject<UClass>(nullptr, *InnerType);
				if (!ObjectClass)
				{
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				OutPinType.PinSubCategoryObject = ObjectClass;
				return true;
			}
			if (Normalized.StartsWith(TEXT("class<")) && TrySplitGenericInner(Normalized, TEXT("class<"), InnerType))
			{
				UClass* ClassType = LoadObject<UClass>(nullptr, *InnerType);
				if (!ClassType)
				{
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
				OutPinType.PinSubCategoryObject = ClassType;
				return true;
			}
			if (Normalized.StartsWith(TEXT("softobject<")) && TrySplitGenericInner(Normalized, TEXT("softobject<"), InnerType))
			{
				UClass* ObjectClass = LoadObject<UClass>(nullptr, *InnerType);
				if (!ObjectClass)
				{
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
				OutPinType.PinSubCategoryObject = ObjectClass;
				return true;
			}
			if (Normalized.StartsWith(TEXT("softclass<")) && TrySplitGenericInner(Normalized, TEXT("softclass<"), InnerType))
			{
				UClass* ClassType = LoadObject<UClass>(nullptr, *InnerType);
				if (!ClassType)
				{
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
				OutPinType.PinSubCategoryObject = ClassType;
				return true;
			}
			if (Normalized.StartsWith(TEXT("struct<")) && TrySplitGenericInner(Normalized, TEXT("struct<"), InnerType))
			{
				UScriptStruct* StructType = LoadObject<UScriptStruct>(nullptr, *InnerType);
				if (!StructType)
				{
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = StructType;
				return true;
			}
			if (Normalized.StartsWith(TEXT("enum<")) && TrySplitGenericInner(Normalized, TEXT("enum<"), InnerType))
			{
				UEnum* EnumType = LoadObject<UEnum>(nullptr, *InnerType);
				if (!EnumType)
				{
					return false;
				}
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				OutPinType.PinSubCategoryObject = EnumType;
				return true;
			}

			return false;
		}

		TSharedRef<FJsonObject> MakeActorListResult(const TArray<AActor*>& Actors)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ActorArray;
			for (const AActor* Actor : Actors)
			{
				ActorArray.Add(MakeShared<FJsonValueObject>(FSololmcpEditorServices::MakeActorReference(Actor)));
			}
			Result->SetArrayField(TEXT("actors"), ActorArray);
			Result->SetNumberField(TEXT("count"), ActorArray.Num());
			return Result;
		}

		TArray<AActor*> ResolveActors(FSololmcpEditorServices& Services, const TArray<FString>& ActorIds, FString& OutError)
		{
			TArray<AActor*> Actors;
			for (const FString& ActorId : ActorIds)
			{
				AActor* Actor = Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return {};
				}
				Actors.Add(Actor);
			}
			return Actors;
		}

		UBlueprint* LoadBlueprintAsset(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
		{
			UObject* Asset = Services.LoadAsset(AssetPath, OutError);
			if (!Asset)
			{
				return nullptr;
			}

			if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
			{
				return Blueprint;
			}

			OutError = TEXT("Asset is not a Blueprint.");
			return nullptr;
		}

		UEdGraph* FindBlueprintGraphByName(UBlueprint* Blueprint, const FString& GraphName)
		{
			if (!Blueprint)
			{
				return nullptr;
			}

			auto MatchesName = [&GraphName](UEdGraph* Graph)
			{
				return Graph && (Graph->GetName() == GraphName || Graph->GetFName().ToString() == GraphName);
			};

			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (MatchesName(Graph))
				{
					return Graph;
				}
			}

			return nullptr;
		}

		UAnimBlueprint* LoadAnimBlueprintAsset(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
		{
			if (UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Services.LoadAsset(AssetPath, OutError)))
			{
				return AnimBlueprint;
			}

			OutError = TEXT("Asset is not an animation blueprint.");
			return nullptr;
		}

		UEdGraph* FindPrimaryAnimBlueprintGraph(UAnimBlueprint* Blueprint)
		{
			if (!Blueprint)
			{
				return nullptr;
			}

			UEdGraph* FirstMatch = nullptr;
			TArray<UEdGraph*> Graphs;
			Graphs.Append(Blueprint->UbergraphPages);
			Graphs.Append(Blueprint->FunctionGraphs);
			Graphs.Append(Blueprint->MacroGraphs);
			Graphs.Append(Blueprint->DelegateSignatureGraphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph || !Graph->GetSchema())
				{
					continue;
				}

				if (Graph->GetSchema()->GetClass() == UAnimationGraphSchema::StaticClass())
				{
					if (!FirstMatch)
					{
						FirstMatch = Graph;
					}

					if (Graph->GetName().Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
					{
						return Graph;
					}
				}
			}

			return FirstMatch;
		}

		TArray<UAnimGraphNode_StateMachineBase*> GetAnimStateMachineNodes(UAnimBlueprint* Blueprint)
		{
			TArray<UAnimGraphNode_StateMachineBase*> Nodes;
			if (Blueprint)
			{
				FBlueprintEditorUtils::GetAllNodesOfClass<UAnimGraphNode_StateMachineBase>(Blueprint, Nodes);
			}
			return Nodes;
		}

		UAnimGraphNode_StateMachineBase* FindAnimStateMachineNode(UAnimBlueprint* Blueprint, const FString& StateMachineName)
		{
			for (UAnimGraphNode_StateMachineBase* Node : GetAnimStateMachineNodes(Blueprint))
			{
				if (!Node)
				{
					continue;
				}

				if (Node->GetStateMachineName() == StateMachineName ||
					(Node->EditorStateMachineGraph && Node->EditorStateMachineGraph->GetName() == StateMachineName))
				{
					return Node;
				}
			}
			return nullptr;
		}

		TArray<UAnimStateNode*> GetAnimStateNodes(UAnimationStateMachineGraph* StateMachineGraph)
		{
			TArray<UAnimStateNode*> Nodes;
			if (!StateMachineGraph)
			{
				return Nodes;
			}

			for (UEdGraphNode* GraphNode : StateMachineGraph->Nodes)
			{
				if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(GraphNode))
				{
					Nodes.Add(StateNode);
				}
			}
			return Nodes;
		}

		UAnimStateNode* FindAnimStateNode(UAnimationStateMachineGraph* StateMachineGraph, const FString& StateName)
		{
			for (UAnimStateNode* StateNode : GetAnimStateNodes(StateMachineGraph))
			{
				if (StateNode && StateNode->GetStateName() == StateName)
				{
					return StateNode;
				}
			}
			return nullptr;
		}

		TArray<UAnimStateTransitionNode*> GetAnimStateTransitions(UAnimationStateMachineGraph* StateMachineGraph)
		{
			TArray<UAnimStateTransitionNode*> Nodes;
			if (!StateMachineGraph)
			{
				return Nodes;
			}

			for (UEdGraphNode* GraphNode : StateMachineGraph->Nodes)
			{
				if (UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(GraphNode))
				{
					Nodes.Add(TransitionNode);
				}
			}
			return Nodes;
		}

		UAnimStateTransitionNode* FindAnimTransition(UAnimationStateMachineGraph* StateMachineGraph, const FString& FromState, const FString& ToState)
		{
			for (UAnimStateTransitionNode* TransitionNode : GetAnimStateTransitions(StateMachineGraph))
			{
				if (!TransitionNode)
				{
					continue;
				}

				const UAnimStateNodeBase* PreviousState = TransitionNode->GetPreviousState();
				const UAnimStateNodeBase* NextState = TransitionNode->GetNextState();
				if (PreviousState && NextState &&
					PreviousState->GetStateName() == FromState &&
					NextState->GetStateName() == ToState)
				{
					return TransitionNode;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> AnimStateMachineTransitionToJson(UAnimStateTransitionNode* TransitionNode)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!TransitionNode)
			{
				return Result;
			}

			const UAnimStateNodeBase* PreviousState = TransitionNode->GetPreviousState();
			const UAnimStateNodeBase* NextState = TransitionNode->GetNextState();
			Result->SetStringField(TEXT("fromState"), PreviousState ? PreviousState->GetStateName() : FString());
			Result->SetStringField(TEXT("toState"), NextState ? NextState->GetStateName() : FString());
			Result->SetStringField(TEXT("nodeGuid"), TransitionNode->NodeGuid.ToString());
			Result->SetStringField(TEXT("graphName"), TransitionNode->GetBoundGraph() ? TransitionNode->GetBoundGraph()->GetName() : FString());
			Result->SetBoolField(TEXT("bidirectional"), TransitionNode->Bidirectional);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
			Result->SetBoolField(TEXT("disabled"), TransitionNode->bDisabled);
#else
			// bDisabled arrived in 5.6; the enabled-state accessor is universal.
			Result->SetBoolField(TEXT("disabled"), !TransitionNode->IsNodeEnabled());
#endif
			Result->SetNumberField(TEXT("priorityOrder"), TransitionNode->PriorityOrder);
			return Result;
		}

		TSharedRef<FJsonObject> AnimBlueprintStateMachinesToJson(UAnimBlueprint* Blueprint)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> MachinesJson;
			for (UAnimGraphNode_StateMachineBase* Node : GetAnimStateMachineNodes(Blueprint))
			{
				if (!Node)
				{
					continue;
				}

				TSharedRef<FJsonObject> MachineJson = MakeShared<FJsonObject>();
				MachineJson->SetStringField(TEXT("name"), Node->GetStateMachineName());
				MachineJson->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
				MachineJson->SetStringField(TEXT("graphName"), Node->EditorStateMachineGraph ? Node->EditorStateMachineGraph->GetName() : FString());
				MachineJson->SetNumberField(TEXT("nodeX"), Node->NodePosX);
				MachineJson->SetNumberField(TEXT("nodeY"), Node->NodePosY);
				MachinesJson.Add(MakeShared<FJsonValueObject>(MachineJson));
			}
			Result->SetArrayField(TEXT("stateMachines"), MachinesJson);
			Result->SetNumberField(TEXT("count"), MachinesJson.Num());
			return Result;
		}

		TSharedRef<FJsonObject> AnimStateMachineStatesToJson(UAnimationStateMachineGraph* StateMachineGraph)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> StatesJson;
			for (UAnimStateNode* StateNode : GetAnimStateNodes(StateMachineGraph))
			{
				if (!StateNode)
				{
					continue;
				}

				TSharedRef<FJsonObject> StateJson = MakeShared<FJsonObject>();
				StateJson->SetStringField(TEXT("name"), StateNode->GetStateName());
				StateJson->SetStringField(TEXT("nodeGuid"), StateNode->NodeGuid.ToString());
				StateJson->SetStringField(TEXT("graphName"), StateNode->BoundGraph ? StateNode->BoundGraph->GetName() : FString());
				StateJson->SetNumberField(TEXT("nodeX"), StateNode->NodePosX);
				StateJson->SetNumberField(TEXT("nodeY"), StateNode->NodePosY);
				StatesJson.Add(MakeShared<FJsonValueObject>(StateJson));
			}
			Result->SetArrayField(TEXT("states"), StatesJson);
			Result->SetNumberField(TEXT("count"), StatesJson.Num());
			return Result;
		}

		TSharedRef<FJsonObject> AnimStateMachineTransitionsToJson(UAnimationStateMachineGraph* StateMachineGraph)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> TransitionsJson;
			for (UAnimStateTransitionNode* TransitionNode : GetAnimStateTransitions(StateMachineGraph))
			{
				TransitionsJson.Add(MakeShared<FJsonValueObject>(AnimStateMachineTransitionToJson(TransitionNode)));
			}
			Result->SetArrayField(TEXT("transitions"), TransitionsJson);
			Result->SetNumberField(TEXT("count"), TransitionsJson.Num());
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintGraphsToJson(UBlueprint* Blueprint)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Graphs;

			auto AppendGraph = [&Graphs](UEdGraph* Graph)
			{
				if (!Graph)
				{
					return;
				}

				TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
				GraphJson->SetStringField(TEXT("name"), Graph->GetName());
				GraphJson->SetStringField(TEXT("class"), Graph->GetClass()->GetPathName());
				Graphs.Add(MakeShared<FJsonValueObject>(GraphJson));
			};

			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				AppendGraph(Graph);
			}
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				AppendGraph(Graph);
			}
			for (UEdGraph* Graph : Blueprint->MacroGraphs)
			{
				AppendGraph(Graph);
			}

			Result->SetArrayField(TEXT("graphs"), Graphs);
			Result->SetNumberField(TEXT("count"), Graphs.Num());
			return Result;
		}

		TArray<UEdGraph*> GetAllBlueprintGraphs(UBlueprint* Blueprint)
		{
			TArray<UEdGraph*> Graphs;
			if (!Blueprint)
			{
				return Graphs;
			}

			Blueprint->GetAllGraphs(Graphs);
			return Graphs;
		}

		template<typename NodeType>
		NodeType* SpawnEditorGraphNode(UEdGraph* Graph, const FVector2f& Location)
		{
			if (!Graph)
			{
				return nullptr;
			}

			NodeType* Node = NewObject<NodeType>(Graph);
			Graph->Modify();
			Graph->AddNode(Node, true, false);
			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();
			Node->AutowireNewNode(nullptr);
			Node->NodePosX = static_cast<int32>(Location.X);
			Node->NodePosY = static_cast<int32>(Location.Y);
			Node->SetFlags(RF_Transactional);
			return Node;
		}

		FString PinDirectionToString(const EEdGraphPinDirection Direction)
		{
			switch (Direction)
			{
			case EGPD_Input:
				return TEXT("input");
			case EGPD_Output:
				return TEXT("output");
			default:
				return TEXT("unknown");
			}
		}

		UEdGraphNode* GetPinOwningNodeSafe(const UEdGraphPin* Pin)
		{
			return Pin ? Pin->GetOwningNodeUnchecked() : nullptr;
		}

		bool IsPinOwnedByNode(const UEdGraphPin* Pin, const UEdGraphNode* ExpectedNode)
		{
			if (!Pin || !ExpectedNode || GetPinOwningNodeSafe(Pin) != ExpectedNode)
			{
				return false;
			}
			return ExpectedNode->Pins.Contains(const_cast<UEdGraphPin*>(Pin));
		}

		bool IsGraphPinStructurallyUsable(const UEdGraphPin* Pin)
		{
			const UEdGraphNode* Owner = GetPinOwningNodeSafe(Pin);
			return Owner && Owner->Pins.Contains(const_cast<UEdGraphPin*>(Pin));
		}

		bool EnsurePinSafeForMutation(
			UEdGraphNode* ExpectedNode,
			UEdGraphPin* Pin,
			const FString& PinLabel,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutError)
		{
			if (!IsPinOwnedByNode(Pin, ExpectedNode))
			{
				OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("damaged_pin_ownership"));
				OutStructured->SetStringField(TEXT("pin_label"), PinLabel);
				OutStructured->SetStringField(TEXT("expected_node_guid"), ExpectedNode ? ExpectedNode->NodeGuid.ToString() : FString());
				OutStructured->SetStringField(TEXT("actual_node_guid"), GetPinOwningNodeSafe(Pin) ? GetPinOwningNodeSafe(Pin)->NodeGuid.ToString() : FString());
				OutError = TEXT("Blueprint pin is damaged or no longer owned by the requested node; mutation was blocked before touching UE graph links.");
				return false;
			}
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!IsGraphPinStructurallyUsable(LinkedPin))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("damaged_pin_links"));
					OutStructured->SetStringField(TEXT("pin_label"), PinLabel);
					OutStructured->SetStringField(TEXT("node_guid"), ExpectedNode->NodeGuid.ToString());
					OutStructured->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
					OutError = TEXT("Blueprint pin has a damaged linked pin; mutation was blocked to avoid an editor assertion.");
					return false;
				}
			}
			return true;
		}

		TSharedRef<FJsonObject> BlueprintPinToJson(const UEdGraphPin* Pin)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Pin)
			{
				return Result;
			}

			Result->SetStringField(TEXT("name"), Pin->PinName.ToString());
			Result->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
			Result->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			Result->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
			Result->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
			Result->SetStringField(TEXT("persistentGuid"), Pin->PersistentGuid.ToString());
			if (Pin->PinType.PinSubCategoryObject.IsValid())
			{
				Result->SetStringField(TEXT("subCategoryObject"), Pin->PinType.PinSubCategoryObject->GetPathName());
			}

			TArray<TSharedPtr<FJsonValue>> LinkedPins;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
				if (!LinkedPin || !LinkedOwner)
				{
					continue;
				}
				TSharedRef<FJsonObject> LinkedPinJson = MakeShared<FJsonObject>();
				LinkedPinJson->SetStringField(TEXT("nodeGuid"), LinkedOwner->NodeGuid.ToString());
				LinkedPinJson->SetStringField(TEXT("pinName"), LinkedPin->PinName.ToString());
				LinkedPinJson->SetStringField(TEXT("pinGuid"), LinkedPin->PersistentGuid.ToString());
				LinkedPins.Add(MakeShared<FJsonValueObject>(LinkedPinJson));
			}
			Result->SetArrayField(TEXT("linkedTo"), LinkedPins);
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintNodeToJson(UEdGraphNode* Node)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Node)
			{
				return Result;
			}

			Result->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
			Result->SetStringField(TEXT("name"), Node->GetName());
			Result->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Result->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			Result->SetNumberField(TEXT("x"), Node->NodePosX);
			Result->SetNumberField(TEXT("y"), Node->NodePosY);

			if (Node->GetGraph())
			{
				Result->SetStringField(TEXT("graph"), Node->GetGraph()->GetName());
			}

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				Pins.Add(MakeShared<FJsonValueObject>(BlueprintPinToJson(Pin)));
			}
			Result->SetArrayField(TEXT("pins"), Pins);
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintNodesToJson(UBlueprint* Blueprint, UEdGraph* Graph = nullptr)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Nodes;

			const TArray<UEdGraph*> Graphs = Graph ? TArray<UEdGraph*>{Graph} : GetAllBlueprintGraphs(Blueprint);
			for (UEdGraph* CurrentGraph : Graphs)
			{
				if (!CurrentGraph)
				{
					continue;
				}
				for (UEdGraphNode* Node : CurrentGraph->Nodes)
				{
					Nodes.Add(MakeShared<FJsonValueObject>(BlueprintNodeToJson(Node)));
				}
			}

			Result->SetArrayField(TEXT("nodes"), Nodes);
			Result->SetNumberField(TEXT("count"), Nodes.Num());
			return Result;
		}

		UEdGraphNode* FindBlueprintNodeByGuid(UBlueprint* Blueprint, const FString& NodeGuidString)
		{
			if (!Blueprint)
			{
				return nullptr;
			}

			FGuid NodeGuid;
			if (!FGuid::Parse(NodeGuidString, NodeGuid))
			{
				return nullptr;
			}

			for (UEdGraph* Graph : GetAllBlueprintGraphs(Blueprint))
			{
				if (!Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node && Node->NodeGuid == NodeGuid)
					{
						return Node;
					}
				}
			}
			return nullptr;
		}

		UEdGraphNode* FindBlueprintNodeByGuidInGraph(UEdGraph* Graph, const FString& NodeGuidString)
		{
			if (!Graph)
			{
				return nullptr;
			}

			FGuid NodeGuid;
			if (!FGuid::Parse(NodeGuidString, NodeGuid))
			{
				return nullptr;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == NodeGuid)
				{
					return Node;
				}
			}
			return nullptr;
		}

		UEdGraphNode* ResolveBlueprintNodeByGuid(UBlueprint* Blueprint, const FString& NodeGuidString, const FString& GraphName)
		{
			if (!Blueprint)
			{
				return nullptr;
			}
			if (!GraphName.IsEmpty())
			{
				return FindBlueprintNodeByGuidInGraph(FindBlueprintGraphByName(Blueprint, GraphName), NodeGuidString);
			}
			return FindBlueprintNodeByGuid(Blueprint, NodeGuidString);
		}

		UEdGraphPin* FindNodePin(UEdGraphNode* Node, const FString& PinName, const FString& PinGuidString = FString(), const TOptional<EEdGraphPinDirection>& Direction = {})
		{
			if (!Node)
			{
				return nullptr;
			}

			FGuid PinGuid;
			const bool bHasGuid = !PinGuidString.IsEmpty() && FGuid::Parse(PinGuidString, PinGuid);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				if (bHasGuid && Pin->PersistentGuid == PinGuid)
				{
					return Pin;
				}
				if (!PinName.IsEmpty() && Pin->PinName.ToString() == PinName)
				{
					if (!Direction.IsSet() || Pin->Direction == Direction.GetValue())
					{
						return Pin;
					}
				}
			}
			return nullptr;
		}

		bool TryGetGraphAndBlueprint(FSololmcpEditorServices& Services, const FString& AssetPath, const FString& GraphName, UBlueprint*& OutBlueprint, UEdGraph*& OutGraph, FString& OutError)
		{
			OutBlueprint = LoadBlueprintAsset(Services, AssetPath, OutError);
			if (!OutBlueprint)
			{
				return false;
			}

			OutGraph = FindBlueprintGraphByName(OutBlueprint, GraphName);
			if (!OutGraph)
			{
				OutError = TEXT("Blueprint graph was not found.");
				return false;
			}
			return true;
		}

		FSomolEditorGraphPosition GetBlueprintNodeLocationFromArguments(const TSharedRef<FJsonObject>& Arguments)
		{
			const int32 NodeX = Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ? Arguments->GetIntegerField(TEXT("node_x")) : 0;
			const int32 NodeY = Arguments->HasTypedField<EJson::Number>(TEXT("node_y")) ? Arguments->GetIntegerField(TEXT("node_y")) : 0;
			return FSomolEditorGraphPosition(static_cast<float>(NodeX), static_cast<float>(NodeY));
		}

		FBlueprintEditor* GetBlueprintEditorForAsset(UBlueprint* Blueprint, FString& OutError)
		{
			if (!Blueprint || !GEditor)
			{
				OutError = TEXT("Blueprint editor is unavailable.");
				return nullptr;
			}

			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditorSubsystem)
			{
				OutError = TEXT("AssetEditorSubsystem is unavailable.");
				return nullptr;
			}

			AssetEditorSubsystem->OpenEditorForAsset(Blueprint);
			if (IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Blueprint, true))
			{
				if (FBlueprintEditor* BlueprintEditor = static_cast<FBlueprintEditor*>(EditorInstance))
				{
					return BlueprintEditor;
				}
			}

			OutError = TEXT("Failed to acquire blueprint editor instance.");
			return nullptr;
		}

		struct FSololmcpBlueprintEditorAccess : FBlueprintEditor
		{
			using FBlueprintEditor::CanCollapseSelectionToFunction;
			using FBlueprintEditor::CanCollapseSelectionToMacro;
			using FBlueprintEditor::CollapseSelectionToFunction;
			using FBlueprintEditor::CollapseSelectionToMacro;
			using FBlueprintEditor::OnCollapseSelectionToFunction;
			using FBlueprintEditor::OnCollapseSelectionToMacro;
		};

		FWidgetBlueprintEditor* GetWidgetBlueprintEditorForAsset(UWidgetBlueprint* WidgetBlueprint, FString& OutError)
		{
			if (!WidgetBlueprint || !GEditor)
			{
				OutError = TEXT("Widget blueprint editor is unavailable.");
				return nullptr;
			}

			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditorSubsystem)
			{
				OutError = TEXT("AssetEditorSubsystem is unavailable.");
				return nullptr;
			}

			AssetEditorSubsystem->OpenEditorForAsset(WidgetBlueprint);
			if (IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(WidgetBlueprint, true))
			{
				return static_cast<FWidgetBlueprintEditor*>(EditorInstance);
			}

			OutError = TEXT("Failed to acquire widget blueprint editor instance.");
			return nullptr;
		}

		TSet<UEdGraphNode*> ResolveBlueprintNodesByGuids(UBlueprint* Blueprint, const TArray<FString>& NodeGuidStrings, FString& OutError)
		{
			TSet<UEdGraphNode*> Nodes;
			for (const FString& NodeGuidString : NodeGuidStrings)
			{
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuidString);
				if (!Node)
				{
					OutError = FString::Printf(TEXT("Blueprint node '%s' was not found."), *NodeGuidString);
					return {};
				}
				Nodes.Add(Node);
			}
			return Nodes;
		}

		TSharedRef<FJsonObject> EventDispatcherGraphToJson(UBlueprint* Blueprint, UEdGraph* Graph)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Blueprint || !Graph)
			{
				return Result;
			}

			Result->SetStringField(TEXT("name"), Graph->GetName());
			Result->SetObjectField(TEXT("graph"), FSololmcpEditorServices::MakeObjectReference(Graph));
			if (FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(Blueprint->SkeletonGeneratedClass, Graph->GetFName()))
			{
				Result->SetStringField(TEXT("delegatePropertyName"), DelegateProperty->GetName());
				Result->SetStringField(TEXT("delegatePropertyOwnerClass"), DelegateProperty->GetOwnerClass() ? DelegateProperty->GetOwnerClass()->GetPathName() : FString());
			}
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintEventDispatchersToJson(UBlueprint* Blueprint)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Dispatchers;
			if (Blueprint)
			{
				for (UEdGraph* DelegateGraph : Blueprint->DelegateSignatureGraphs)
				{
					if (DelegateGraph)
					{
						Dispatchers.Add(MakeShared<FJsonValueObject>(EventDispatcherGraphToJson(Blueprint, DelegateGraph)));
					}
				}
			}
			Result->SetArrayField(TEXT("eventDispatchers"), Dispatchers);
			Result->SetNumberField(TEXT("count"), Dispatchers.Num());
			return Result;
		}

		FMulticastDelegateProperty* ResolveBlueprintEventDispatcherProperty(UBlueprint* Blueprint, const FString& DispatcherName, FString& OutError)
		{
			if (!Blueprint || !Blueprint->SkeletonGeneratedClass)
			{
				OutError = TEXT("Blueprint skeleton class is unavailable.");
				return nullptr;
			}

			if (FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(Blueprint->SkeletonGeneratedClass, *DispatcherName))
			{
				return DelegateProperty;
			}

			OutError = TEXT("Event dispatcher property was not found.");
			return nullptr;
		}

		UEdGraphNode* SpawnBlueprintNodeByClass(UEdGraph* Graph, TSubclassOf<UEdGraphNode> NodeClass, const FSomolEditorGraphPosition& Location, const UBlueprintNodeSpawner::FCustomizeNodeDelegate& CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate())
		{
			if (!Graph || !*NodeClass)
			{
				return nullptr;
			}

			UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(NodeClass, nullptr, CustomizeNodeDelegate);
			return NodeSpawner ? NodeSpawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(Location)) : nullptr;
		}

		UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph)
		{
			if (!Graph)
			{
				return nullptr;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
				{
					return EntryNode;
				}
			}

			return nullptr;
		}

		UK2Node_CustomEvent* FindCustomEventNode(UEdGraph* Graph)
		{
			if (!Graph)
			{
				return nullptr;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(Node))
				{
					return EventNode;
				}
			}

			return nullptr;
		}

		TSharedPtr<FUserPinInfo> FindUserDefinedPinInfo(UK2Node_EditablePinBase* Node, const FString& PinName)
		{
			if (!Node)
			{
				return nullptr;
			}

			for (const TSharedPtr<FUserPinInfo>& PinInfo : Node->UserDefinedPins)
			{
				if (PinInfo.IsValid() && PinInfo->PinName.ToString() == PinName)
				{
					return PinInfo;
				}
			}

			return nullptr;
		}

		bool TryParseBlueprintPinDirection(const FString& DirectionName, EEdGraphPinDirection& OutDirection)
		{
			if (DirectionName == TEXT("input"))
			{
				OutDirection = EGPD_Input;
				return true;
			}
			if (DirectionName == TEXT("output"))
			{
				OutDirection = EGPD_Output;
				return true;
			}
			return false;
		}

		UK2Node_EditablePinBase* ResolveBlueprintSignatureNode(UBlueprint* Blueprint, UEdGraph* Graph, const FString& NodeGuidString, FString& OutError)
		{
			if (!Blueprint || !Graph)
			{
				OutError = TEXT("Blueprint graph was not found.");
				return nullptr;
			}

			if (!NodeGuidString.IsEmpty())
			{
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuidString);
				if (UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(Node))
				{
					return EditableNode;
				}
				OutError = TEXT("node_guid does not resolve to an editable signature node.");
				return nullptr;
			}

			if (UK2Node_FunctionEntry* FunctionEntry = FindFunctionEntryNode(Graph))
			{
				return FunctionEntry;
			}

			TArray<UK2Node_CustomEvent*> CustomEvents;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(Node))
				{
					CustomEvents.Add(EventNode);
				}
			}

			if (CustomEvents.Num() == 1)
			{
				return CustomEvents[0];
			}

			OutError = CustomEvents.Num() > 1
				? TEXT("Multiple custom event nodes were found. Please provide node_guid.")
				: TEXT("No editable function entry or custom event node was found for the graph.");
			return nullptr;
		}

		TSharedRef<FJsonObject> BlueprintSignaturePinsToJson(UK2Node_EditablePinBase* Node)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> PinsJson;
			if (Node)
			{
				for (const TSharedPtr<FUserPinInfo>& PinInfo : Node->UserDefinedPins)
				{
					if (!PinInfo.IsValid())
					{
						continue;
					}

					TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
					PinJson->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
					PinJson->SetStringField(TEXT("defaultValue"), PinInfo->PinDefaultValue);
					PinJson->SetStringField(TEXT("direction"), PinInfo->DesiredPinDirection == EGPD_Output ? TEXT("output") : TEXT("input"));
					PinJson->SetStringField(TEXT("category"), PinInfo->PinType.PinCategory.ToString());
					if (PinInfo->PinType.PinSubCategoryObject.IsValid())
					{
						PinJson->SetStringField(TEXT("subCategoryObject"), PinInfo->PinType.PinSubCategoryObject->GetPathName());
					}
					PinsJson.Add(MakeShared<FJsonValueObject>(PinJson));
				}
			}
			Result->SetArrayField(TEXT("pins"), PinsJson);
			Result->SetNumberField(TEXT("count"), PinsJson.Num());
			return Result;
		}

		void ReorderBlueprintUserPin(UK2Node_EditablePinBase* Node, const FString& PinName, int32 TargetIndex, FString& OutError)
		{
			if (!Node)
			{
				OutError = TEXT("Editable signature node was not found.");
				return;
			}

			const int32 CurrentIndex = Node->UserDefinedPins.IndexOfByPredicate([&PinName](const TSharedPtr<FUserPinInfo>& PinInfo)
			{
				return PinInfo.IsValid() && PinInfo->PinName.ToString() == PinName;
			});

			if (!Node->UserDefinedPins.IsValidIndex(CurrentIndex))
			{
				OutError = TEXT("Signature pin was not found.");
				return;
			}

			if (TargetIndex < 0 || TargetIndex >= Node->UserDefinedPins.Num())
			{
				OutError = TEXT("target_index is out of range.");
				return;
			}

			const TSharedPtr<FUserPinInfo> PinInfo = Node->UserDefinedPins[CurrentIndex];
			Node->UserDefinedPins.RemoveAt(CurrentIndex);
			Node->UserDefinedPins.Insert(PinInfo, TargetIndex);
			Node->ReconstructNode();
		}

		TSharedRef<FJsonObject> BlueprintVariableDescriptionToJson(const FBPVariableDescription& Variable)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), Variable.VarName.ToString());
			Result->SetStringField(TEXT("friendlyName"), Variable.FriendlyName);
			Result->SetStringField(TEXT("category"), Variable.Category.ToString());
			Result->SetStringField(TEXT("defaultValue"), Variable.DefaultValue);
			Result->SetStringField(TEXT("tooltip"),
				Variable.HasMetaData(FBlueprintMetadata::MD_Tooltip)
					? Variable.GetMetaData(FBlueprintMetadata::MD_Tooltip)
					: FString());
			Result->SetBoolField(TEXT("transient"), Variable.PropertyFlags & CPF_Transient);
			Result->SetBoolField(TEXT("saveGame"), Variable.PropertyFlags & CPF_SaveGame);
			Result->SetBoolField(TEXT("blueprintReadOnly"), Variable.PropertyFlags & CPF_BlueprintReadOnly);
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintLocalVariablesToJson(UBlueprint* Blueprint, UEdGraph* Graph)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> VariablesJson;
			if (Blueprint && Graph)
			{
				if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(Graph))
				{
					for (const FBPVariableDescription& Variable : EntryNode->LocalVariables)
					{
						VariablesJson.Add(MakeShared<FJsonValueObject>(BlueprintVariableDescriptionToJson(Variable)));
					}
				}
			}
			Result->SetArrayField(TEXT("variables"), VariablesJson);
			Result->SetNumberField(TEXT("count"), VariablesJson.Num());
			return Result;
		}

		UMaterial* LoadMaterialAsset(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
		{
			UObject* Asset = Services.LoadAsset(AssetPath, OutError);
			if (!Asset)
			{
				return nullptr;
			}

			if (UMaterial* Material = Cast<UMaterial>(Asset))
			{
				return Material;
			}

			// Audit round 9 (group C): include actual class name so callers can tell a
			// MaterialInstanceConstant from a non-material asset; many "Asset is not a
			// material" failures were silent-class-mismatch on instance vs base material.
			OutError = FString::Printf(TEXT("Asset is not a UMaterial (got %s) at %s"),
				*Asset->GetClass()->GetName(), *AssetPath);
			return nullptr;
		}

		int32 FindMaterialExpressionIndex(const UMaterial* Material, const UMaterialExpression* Expression)
		{
			if (!Material || !Expression)
			{
				return INDEX_NONE;
			}

			const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
			for (int32 Index = 0; Index < Expressions.Num(); ++Index)
			{
				if (Expressions[Index] == Expression)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}

		UMaterialExpression* FindMaterialExpressionByIndex(UMaterial* Material, int32 ExpressionIndex)
		{
			if (!Material)
			{
				return nullptr;
			}

			const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
			return Expressions.IsValidIndex(ExpressionIndex) ? Expressions[ExpressionIndex] : nullptr;
		}

		UMaterialExpression* FindMaterialExpressionByGuid(UMaterial* Material, const FString& ExpressionGuidString)
		{
			if (!Material || ExpressionGuidString.IsEmpty())
			{
				return nullptr;
			}

			FGuid ExpressionGuid;
			if (!FGuid::Parse(ExpressionGuidString, ExpressionGuid))
			{
				return nullptr;
			}

			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (Expression && Expression->MaterialExpressionGuid == ExpressionGuid)
				{
					return Expression;
				}
			}
			return nullptr;
		}

		UMaterialExpression* ResolveMaterialExpressionFromArguments(UMaterial* Material, const TSharedRef<FJsonObject>& Arguments, const FString& IndexField, const FString& GuidField, FString& OutError)
		{
			FString ExpressionGuidString;
			if (Arguments->TryGetStringField(GuidField, ExpressionGuidString) && !ExpressionGuidString.IsEmpty())
			{
				if (UMaterialExpression* Expression = FindMaterialExpressionByGuid(Material, ExpressionGuidString))
				{
					return Expression;
				}
				OutError = FString::Printf(TEXT("%s is invalid or not found."), *GuidField);
				return nullptr;
			}

			int32 ExpressionIndex = INDEX_NONE;
			if (!Arguments->TryGetNumberField(IndexField, ExpressionIndex))
			{
				OutError = FString::Printf(TEXT("Missing %s or %s."), *IndexField, *GuidField);
				return nullptr;
			}

			if (UMaterialExpression* Expression = FindMaterialExpressionByIndex(Material, ExpressionIndex))
			{
				return Expression;
			}

			OutError = FString::Printf(TEXT("%s is invalid."), *IndexField);
			return nullptr;
		}

		int32 FindMaterialFunctionExpressionIndex(const UMaterialFunctionInterface* MaterialFunction, const UMaterialExpression* Expression)
		{
			if (!MaterialFunction || !Expression)
			{
				return INDEX_NONE;
			}

			const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = MaterialFunction->GetExpressions();
			for (int32 Index = 0; Index < Expressions.Num(); ++Index)
			{
				if (Expressions[Index] == Expression)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}

		UMaterialExpression* FindMaterialFunctionExpressionByIndex(UMaterialFunctionInterface* MaterialFunction, int32 ExpressionIndex)
		{
			if (!MaterialFunction)
			{
				return nullptr;
			}

			const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = MaterialFunction->GetExpressions();
			return Expressions.IsValidIndex(ExpressionIndex) ? Expressions[ExpressionIndex] : nullptr;
		}

		TSharedRef<FJsonObject> MaterialExpressionToJson(UMaterial* Material, UMaterialExpression* Expression)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("index"), FindMaterialExpressionIndex(Material, Expression));
			Result->SetStringField(TEXT("guid"), Expression ? Expression->MaterialExpressionGuid.ToString() : FString());
			Result->SetStringField(TEXT("name"), Expression ? Expression->GetName() : FString());
			Result->SetStringField(TEXT("class"), Expression ? Expression->GetClass()->GetPathName() : FString());
			Result->SetNumberField(TEXT("x"), Expression ? Expression->MaterialExpressionEditorX : 0);
			Result->SetNumberField(TEXT("y"), Expression ? Expression->MaterialExpressionEditorY : 0);
			if (Expression)
			{
				Result->SetStringField(TEXT("parameterName"), Expression->GetParameterName().ToString());
				TArray<TSharedPtr<FJsonValue>> InputNamesJson;
				for (const FString& InputName : UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expression))
				{
					InputNamesJson.Add(MakeShared<FJsonValueString>(InputName));
				}
				Result->SetArrayField(TEXT("inputNames"), InputNamesJson);

				TArray<TSharedPtr<FJsonValue>> ConnectedInputs;
				for (UMaterialExpression* InputExpression : UMaterialEditingLibrary::GetInputsForMaterialExpression(Material, Expression))
				{
					if (!InputExpression)
					{
						continue;
					}

					TSharedRef<FJsonObject> InputJson = MakeShared<FJsonObject>();
					InputJson->SetNumberField(TEXT("expressionIndex"), FindMaterialExpressionIndex(Material, InputExpression));
					InputJson->SetStringField(TEXT("expressionName"), InputExpression->GetName());

					FString OutputName;
					if (UMaterialEditingLibrary::GetInputNodeOutputNameForMaterialExpression(Expression, InputExpression, OutputName))
					{
						InputJson->SetStringField(TEXT("outputName"), OutputName);
					}

					ConnectedInputs.Add(MakeShared<FJsonValueObject>(InputJson));
				}
				Result->SetArrayField(TEXT("connectedInputs"), ConnectedInputs);
			}
			return Result;
		}

		TSharedRef<FJsonObject> MaterialFunctionExpressionToJson(UMaterialFunctionInterface* MaterialFunction, UMaterialExpression* Expression)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("index"), FindMaterialFunctionExpressionIndex(MaterialFunction, Expression));
			Result->SetStringField(TEXT("name"), Expression ? Expression->GetName() : FString());
			Result->SetStringField(TEXT("class"), Expression ? Expression->GetClass()->GetPathName() : FString());
			Result->SetNumberField(TEXT("x"), Expression ? Expression->MaterialExpressionEditorX : 0);
			Result->SetNumberField(TEXT("y"), Expression ? Expression->MaterialExpressionEditorY : 0);
			if (Expression)
			{
				Result->SetStringField(TEXT("parameterName"), Expression->GetParameterName().ToString());
			}
			return Result;
		}

		TSharedRef<FJsonObject> MaterialExpressionsToJson(UMaterial* Material)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ExpressionsJson;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				ExpressionsJson.Add(MakeShared<FJsonValueObject>(MaterialExpressionToJson(Material, Expression)));
			}
			Result->SetArrayField(TEXT("expressions"), ExpressionsJson);
			Result->SetNumberField(TEXT("count"), ExpressionsJson.Num());
			return Result;
		}

		TArray<FString> GetSupportedMaterialPropertyNames()
		{
			return {
				TEXT("BaseColor"), TEXT("Metallic"), TEXT("Specular"), TEXT("Roughness"), TEXT("EmissiveColor"),
				TEXT("Opacity"), TEXT("OpacityMask"), TEXT("Normal"), TEXT("WorldPositionOffset"), TEXT("AmbientOcclusion"),
				TEXT("Refraction"), TEXT("Anisotropy"), TEXT("Tangent"), TEXT("Displacement"), TEXT("SubsurfaceColor"),
				TEXT("ClearCoat"), TEXT("ClearCoatRoughness"), TEXT("PixelDepthOffset")
			};
		}

		FExpressionInput* GetMaterialPropertyInputByName(UMaterial* Material, const FString& PropertyName)
		{
			if (!Material)
			{
				return nullptr;
			}
			static const TMap<FString, EMaterialProperty> PropertyMap = {
				{TEXT("BaseColor"), MP_BaseColor},
				{TEXT("Metallic"), MP_Metallic},
				{TEXT("Specular"), MP_Specular},
				{TEXT("Roughness"), MP_Roughness},
				{TEXT("EmissiveColor"), MP_EmissiveColor},
				{TEXT("Opacity"), MP_Opacity},
				{TEXT("OpacityMask"), MP_OpacityMask},
				{TEXT("Normal"), MP_Normal},
				{TEXT("WorldPositionOffset"), MP_WorldPositionOffset},
				{TEXT("AmbientOcclusion"), MP_AmbientOcclusion},
				{TEXT("Refraction"), MP_Refraction},
				{TEXT("Anisotropy"), MP_Anisotropy},
				{TEXT("Tangent"), MP_Tangent},
				{TEXT("Displacement"), MP_Displacement},
				{TEXT("SubsurfaceColor"), MP_SubsurfaceColor},
				{TEXT("ClearCoat"), MP_CustomData0},
				{TEXT("ClearCoatRoughness"), MP_CustomData1},
				{TEXT("PixelDepthOffset"), MP_PixelDepthOffset}
			};
			if (const EMaterialProperty* Property = PropertyMap.Find(PropertyName))
			{
				return Material->GetExpressionInputForProperty(*Property);
			}
			return nullptr;
		}

		UFunction* FindFunctionOnClassByLooseName(UClass* OwnerClass, const FString& FunctionName)
		{
			if (!OwnerClass || FunctionName.IsEmpty())
			{
				return nullptr;
			}

			if (UFunction* Direct = OwnerClass->FindFunctionByName(*FunctionName))
			{
				return Direct;
			}

			const FString NormalizedNeedle = FunctionName.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
			for (TFieldIterator<UFunction> It(OwnerClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Candidate = *It;
				if (!Candidate)
				{
					continue;
				}
				if (Candidate->GetName().Equals(FunctionName, ESearchCase::IgnoreCase)
					|| Candidate->GetDisplayNameText().ToString().Equals(FunctionName, ESearchCase::IgnoreCase))
				{
					return Candidate;
				}
				const FString NormalizedName = Candidate->GetName().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
				const FString NormalizedDisplay = Candidate->GetDisplayNameText().ToString().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
				if (NormalizedName == NormalizedNeedle || NormalizedDisplay == NormalizedNeedle)
				{
					return Candidate;
				}
			}

			return nullptr;
		}

		TSharedRef<FJsonObject> BlueprintCallFunctionNodeDetailsToJson(UK2Node_CallFunction* CallNode)
		{
			TSharedRef<FJsonObject> Result = BlueprintNodeToJson(CallNode);
			if (!CallNode)
			{
				return Result;
			}

			UFunction* TargetFunction = CallNode->GetTargetFunction();
			Result->SetStringField(TEXT("function_name"), CallNode->FunctionReference.GetMemberName().ToString());
			Result->SetStringField(TEXT("function_owner"), CallNode->FunctionReference.GetMemberParentClass()
				? CallNode->FunctionReference.GetMemberParentClass()->GetPathName()
				: FString());
			Result->SetStringField(TEXT("resolved_function"), TargetFunction ? TargetFunction->GetPathName() : FString());
			Result->SetStringField(TEXT("resolved_owner"), TargetFunction && TargetFunction->GetOwnerClass()
				? TargetFunction->GetOwnerClass()->GetPathName()
				: FString());
			Result->SetBoolField(TEXT("resolved"), TargetFunction != nullptr);
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintMacroInstanceDetailsToJson(UK2Node_MacroInstance* MacroNode)
		{
			TSharedRef<FJsonObject> Result = BlueprintNodeToJson(MacroNode);
			if (!MacroNode)
			{
				return Result;
			}

			UEdGraph* MacroGraph = MacroNode->GetMacroGraph();
			Result->SetStringField(TEXT("macro_graph"), MacroGraph ? MacroGraph->GetName() : FString());
			Result->SetStringField(TEXT("macro_graph_path"), MacroGraph ? MacroGraph->GetPathName() : FString());
			Result->SetBoolField(TEXT("macro_graph_resolved"), MacroGraph != nullptr);
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintDelegateNodeDetailsToJson(UK2Node_BaseMCDelegate* DelegateNode)
		{
			TSharedRef<FJsonObject> Result = BlueprintNodeToJson(DelegateNode);
			if (!DelegateNode)
			{
				return Result;
			}

			Result->SetStringField(TEXT("delegate_name"), DelegateNode->GetPropertyName().ToString());
			if (FProperty* Property = DelegateNode->GetProperty())
			{
				Result->SetStringField(TEXT("delegate_property"), Property->GetPathName());
				Result->SetStringField(TEXT("delegate_owner_class"), Property->GetOwnerClass() ? Property->GetOwnerClass()->GetPathName() : FString());
			}
			if (UFunction* Signature = DelegateNode->GetDelegateSignature(true))
			{
				Result->SetStringField(TEXT("delegate_signature"), Signature->GetPathName());
			}
			Result->SetBoolField(TEXT("resolved"), DelegateNode->GetProperty() != nullptr);
			return Result;
		}

		FMulticastDelegateProperty* ResolveMulticastDelegatePropertyOnClass(UClass* OwnerClass, const FString& DispatcherName)
		{
			if (!OwnerClass || DispatcherName.IsEmpty())
			{
				return nullptr;
			}

			const FString NormalizedQuery = DispatcherName.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
			for (TFieldIterator<FMulticastDelegateProperty> It(OwnerClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				FMulticastDelegateProperty* Property = *It;
				if (!Property)
				{
					continue;
				}
				const FString Name = Property->GetName();
				const FString DisplayName = Property->GetDisplayNameText().ToString();
				const FString NormalizedName = Name.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
				const FString NormalizedDisplay = DisplayName.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
				if (Name == DispatcherName
					|| DisplayName == DispatcherName
					|| NormalizedName == NormalizedQuery
					|| NormalizedDisplay == NormalizedQuery)
				{
					return Property;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> BlueprintVariableNodeDetailsToJson(UK2Node_Variable* VariableNode)
		{
			TSharedRef<FJsonObject> Result = BlueprintNodeToJson(VariableNode);
			if (!VariableNode)
			{
				return Result;
			}

			FProperty* Property = VariableNode->GetPropertyForVariable();
			Result->SetStringField(TEXT("variable_name"), VariableNode->GetVarNameString());
			Result->SetStringField(TEXT("variable_owner"), VariableNode->VariableReference.GetMemberParentClass()
				? VariableNode->VariableReference.GetMemberParentClass()->GetPathName()
				: FString());
			Result->SetBoolField(TEXT("self_context"), VariableNode->VariableReference.IsSelfContext());
			Result->SetStringField(TEXT("resolved_property"), Property ? Property->GetPathName() : FString());
			Result->SetStringField(TEXT("resolved_owner"), Property && Property->GetOwnerClass()
				? Property->GetOwnerClass()->GetPathName()
				: FString());
			Result->SetBoolField(TEXT("resolved"), Property != nullptr);
			return Result;
		}

		FProperty* ResolvePropertyOnClass(UClass* OwnerClass, const FString& VariableName)
		{
			if (!OwnerClass || VariableName.IsEmpty())
			{
				return nullptr;
			}

			if (FProperty* Direct = FindFProperty<FProperty>(OwnerClass, *VariableName))
			{
				return Direct;
			}

			const FString NormalizedQuery = VariableName.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
			for (TFieldIterator<FProperty> It(OwnerClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}
				const FString Name = Property->GetName();
				const FString DisplayName = Property->GetDisplayNameText().ToString();
				const FString NormalizedName = Name.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
				const FString NormalizedDisplay = DisplayName.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
				if (Name == VariableName
					|| DisplayName == VariableName
					|| NormalizedName == NormalizedQuery
					|| NormalizedDisplay == NormalizedQuery)
				{
					return Property;
				}
			}
			return nullptr;
		}

		void AddStringArrayField(TSharedRef<FJsonObject> Object, const FString& FieldName, const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			Object->SetArrayField(FieldName, JsonValues);
		}

		bool ContainsAnyToken(const FString& Text, const TArray<FString>& Tokens)
		{
			for (const FString& Token : Tokens)
			{
				if (Text.Contains(Token))
				{
					return true;
				}
			}
			return false;
		}

		TSharedRef<FJsonObject> DetectMaterialTextureRoleJson(
			UTexture* Texture,
			const FString& ConnectedProperty,
			const FString& ParameterName,
			const FString& SamplerTypeName)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const FString TexturePath = Texture ? Texture->GetPathName() : FString();
			const FString TextureName = Texture ? Texture->GetName() : FString();
			const FString ProbeText = FString::Printf(TEXT("%s %s %s %s"),
				*TexturePath.ToLower(), *TextureName.ToLower(), *ParameterName.ToLower(), *ConnectedProperty.ToLower());

			TMap<FString, double> Scores;
			TArray<FString> Reasons;
			auto AddScore = [&Scores, &Reasons](const FString& Role, double Amount, const FString& Reason)
			{
				Scores.FindOrAdd(Role) += Amount;
				if (!Reason.IsEmpty())
				{
					Reasons.Add(FString::Printf(TEXT("%s:%s"), *Role, *Reason));
				}
			};

			if (!ConnectedProperty.IsEmpty())
			{
				if (ConnectedProperty == TEXT("BaseColor")) AddScore(TEXT("base_color"), 0.70, TEXT("connected_to_BaseColor"));
				else if (ConnectedProperty == TEXT("Normal")) AddScore(TEXT("normal"), 0.80, TEXT("connected_to_Normal"));
				else if (ConnectedProperty == TEXT("Roughness")) AddScore(TEXT("roughness"), 0.75, TEXT("connected_to_Roughness"));
				else if (ConnectedProperty == TEXT("Metallic")) AddScore(TEXT("metallic"), 0.75, TEXT("connected_to_Metallic"));
				else if (ConnectedProperty == TEXT("AmbientOcclusion")) AddScore(TEXT("ambient_occlusion"), 0.75, TEXT("connected_to_AmbientOcclusion"));
				else if (ConnectedProperty == TEXT("Opacity") || ConnectedProperty == TEXT("OpacityMask")) AddScore(TEXT("opacity"), 0.75, TEXT("connected_to_opacity"));
				else if (ConnectedProperty == TEXT("EmissiveColor")) AddScore(TEXT("emissive"), 0.75, TEXT("connected_to_EmissiveColor"));
				else if (ConnectedProperty == TEXT("Displacement") || ConnectedProperty == TEXT("WorldPositionOffset")) AddScore(TEXT("height"), 0.65, TEXT("connected_to_displacement_path"));
			}

			if (ContainsAnyToken(ProbeText, {TEXT("_bc"), TEXT("_basecolor"), TEXT("_base_color"), TEXT("_albedo"), TEXT("_diff"), TEXT("_diffuse"), TEXT("basecolor"), TEXT("albedo")}))
			{
				AddScore(TEXT("base_color"), 0.45, TEXT("name_token"));
			}
			if (ContainsAnyToken(ProbeText, {TEXT("_n"), TEXT("_nm"), TEXT("_normal"), TEXT("_nrm"), TEXT("normal")}))
			{
				AddScore(TEXT("normal"), 0.50, TEXT("name_token"));
			}
			if (ContainsAnyToken(ProbeText, {TEXT("_r"), TEXT("_rough"), TEXT("_roughness"), TEXT("roughness")}))
			{
				AddScore(TEXT("roughness"), 0.45, TEXT("name_token"));
			}
			if (ContainsAnyToken(ProbeText, {TEXT("_m"), TEXT("_metal"), TEXT("_metallic"), TEXT("metallic")}))
			{
				AddScore(TEXT("metallic"), 0.40, TEXT("name_token"));
			}
			if (ContainsAnyToken(ProbeText, {TEXT("_ao"), TEXT("_occ"), TEXT("_occlusion"), TEXT("ambientocclusion"), TEXT("ambient_occlusion")}))
			{
				AddScore(TEXT("ambient_occlusion"), 0.45, TEXT("name_token"));
			}
			if (ContainsAnyToken(ProbeText, {TEXT("_orm"), TEXT("_rma"), TEXT("_mrao"), TEXT("_mask"), TEXT("_packed"), TEXT("occlusionroughnessmetallic")}))
			{
				AddScore(TEXT("packed_orm"), 0.55, TEXT("packed_name_token"));
			}
			if (ContainsAnyToken(ProbeText, {TEXT("_h"), TEXT("_height"), TEXT("_disp"), TEXT("_displacement"), TEXT("height")}))
			{
				AddScore(TEXT("height"), 0.40, TEXT("name_token"));
			}
			if (ContainsAnyToken(ProbeText, {TEXT("_e"), TEXT("_emit"), TEXT("_emissive"), TEXT("emissive")}))
			{
				AddScore(TEXT("emissive"), 0.40, TEXT("name_token"));
			}
			if (ContainsAnyToken(ProbeText, {TEXT("_opacity"), TEXT("_alpha"), TEXT("_mask"), TEXT("opacity")}))
			{
				AddScore(TEXT("opacity"), 0.35, TEXT("name_token"));
			}

			if (Texture)
			{
				if (Texture->CompressionSettings == TC_Normalmap)
				{
					AddScore(TEXT("normal"), 0.55, TEXT("TC_Normalmap"));
				}
				if (Texture->CompressionSettings == TC_Masks)
				{
					AddScore(TEXT("packed_orm"), 0.30, TEXT("TC_Masks"));
				}
				if (Texture->SRGB)
				{
					AddScore(TEXT("base_color"), 0.15, TEXT("sRGB"));
					AddScore(TEXT("emissive"), 0.05, TEXT("sRGB"));
				}
				else
				{
					AddScore(TEXT("data_map"), 0.12, TEXT("linear_texture"));
				}
			}
			if (SamplerTypeName.Contains(TEXT("Normal")))
			{
				AddScore(TEXT("normal"), 0.35, TEXT("normal_sampler"));
			}

			FString BestRole = TEXT("unknown");
			double BestScore = 0.0;
			for (const TPair<FString, double>& Pair : Scores)
			{
				if (Pair.Value > BestScore)
				{
					BestRole = Pair.Key;
					BestScore = Pair.Value;
				}
			}
			const double Confidence = FMath::Clamp(BestScore, 0.0, 1.0);

			Result->SetStringField(TEXT("role"), BestRole);
			Result->SetNumberField(TEXT("confidence"), FMath::RoundToDouble(Confidence * 100.0) / 100.0);
			Result->SetStringField(TEXT("texture_path"), TexturePath);
			Result->SetStringField(TEXT("texture_name"), TextureName);
			Result->SetStringField(TEXT("parameter_name"), ParameterName);
			Result->SetStringField(TEXT("connected_property"), ConnectedProperty);
			Result->SetStringField(TEXT("sampler_type"), SamplerTypeName);
			Result->SetBoolField(TEXT("is_srgb"), Texture ? Texture->SRGB : false);
			Result->SetStringField(TEXT("compression_settings"), Texture
				? StaticEnum<TextureCompressionSettings>()->GetNameStringByValue(static_cast<int64>(Texture->CompressionSettings))
				: FString());
			AddStringArrayField(Result, TEXT("evidence"), Reasons);

			if (BestRole == TEXT("packed_orm"))
			{
				TSharedRef<FJsonObject> Channels = MakeShared<FJsonObject>();
				Channels->SetStringField(TEXT("r"), TEXT("ambient_occlusion"));
				Channels->SetStringField(TEXT("g"), TEXT("roughness"));
				Channels->SetStringField(TEXT("b"), TEXT("metallic"));
				Channels->SetStringField(TEXT("a"), TEXT("optional_mask"));
				Result->SetObjectField(TEXT("channel_roles"), Channels);
			}
			return Result;
		}

		TSharedRef<FJsonObject> MakeMaterialConnectionJson(UMaterial* Material, const FString& PropertyName, const FExpressionInput* Input)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("property"), PropertyName);
			Result->SetBoolField(TEXT("connected"), Input && Input->Expression);
			if (Input && Input->Expression)
			{
				Result->SetNumberField(TEXT("expression_index"), FindMaterialExpressionIndex(Material, Input->Expression));
				Result->SetStringField(TEXT("expression_guid"), Input->Expression->MaterialExpressionGuid.ToString());
				Result->SetStringField(TEXT("expression_name"), Input->Expression->GetName());
				Result->SetStringField(TEXT("expression_class"), Input->Expression->GetClass()->GetPathName());
				Result->SetNumberField(TEXT("output_index"), Input->OutputIndex);
			}
			return Result;
		}

		TSharedRef<FJsonObject> MaterialFunctionExpressionsToJson(UMaterialFunctionInterface* MaterialFunction)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ExpressionsJson;
			if (MaterialFunction)
			{
				for (UMaterialExpression* Expression : MaterialFunction->GetExpressions())
				{
					ExpressionsJson.Add(MakeShared<FJsonValueObject>(MaterialFunctionExpressionToJson(MaterialFunction, Expression)));
				}
			}
			Result->SetArrayField(TEXT("expressions"), ExpressionsJson);
			Result->SetNumberField(TEXT("count"), ExpressionsJson.Num());
			return Result;
		}

		UMaterialFunction* LoadMaterialFunctionAsset(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
		{
			UObject* Asset = Services.LoadAsset(AssetPath, OutError);
			if (!Asset)
			{
				return nullptr;
			}

			if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Asset))
			{
				return MaterialFunction;
			}

			// Audit round 9 (group C): include actual class name in diagnostic
			// (mirrors LoadMaterialAsset above).
			OutError = FString::Printf(TEXT("Asset is not a UMaterialFunction (got %s) at %s"),
				*Asset->GetClass()->GetName(), *AssetPath);
			return nullptr;
		}

		bool TryParseMaterialProperty(const FString& PropertyName, EMaterialProperty& OutProperty, const UMaterial* Material = nullptr)
		{
			static const TMap<FString, EMaterialProperty> PropertyMap = {
				{TEXT("BaseColor"), MP_BaseColor},
				{TEXT("Metallic"), MP_Metallic},
				{TEXT("Specular"), MP_Specular},
				{TEXT("Roughness"), MP_Roughness},
				{TEXT("EmissiveColor"), MP_EmissiveColor},
				{TEXT("Opacity"), MP_Opacity},
				{TEXT("OpacityMask"), MP_OpacityMask},
				{TEXT("Normal"), MP_Normal},
				{TEXT("WorldPositionOffset"), MP_WorldPositionOffset},
				{TEXT("AmbientOcclusion"), MP_AmbientOcclusion},
				{TEXT("Refraction"), MP_Refraction},
				{TEXT("Anisotropy"), MP_Anisotropy},
				{TEXT("Tangent"), MP_Tangent},
				{TEXT("Displacement"), MP_Displacement},
				{TEXT("SubsurfaceColor"), MP_SubsurfaceColor},
				{TEXT("ClearCoat"), MP_CustomData0},
				{TEXT("ClearCoatRoughness"), MP_CustomData1},
				{TEXT("PixelDepthOffset"), MP_PixelDepthOffset}
			};

			FString Stripped = PropertyName.TrimStartAndEnd();
			Stripped.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
			const FString Normalized = Stripped.ToLower();
			const FString Compact = Normalized.Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
			static const TMap<FString, FString> Aliases = {
				{TEXT("base_color"), TEXT("BaseColor")},
				{TEXT("emissive"), TEXT("EmissiveColor")},
				{TEXT("wpo"), TEXT("WorldPositionOffset")},
				{TEXT("ao"), TEXT("AmbientOcclusion")},
				{TEXT("clearcoat"), TEXT("ClearCoat")},
				{TEXT("clearcoatroughness"), TEXT("ClearCoatRoughness")},
				{TEXT("pdo"), TEXT("PixelDepthOffset")}
			};

			for (const TPair<FString, EMaterialProperty>& Pair : PropertyMap)
			{
				if (Pair.Key.ToLower() == Normalized)
				{
					OutProperty = Pair.Value;
					return true;
				}
			}

			if (const FString* Canonical = Aliases.Find(Normalized))
			{
				if (const EMaterialProperty* Found = PropertyMap.Find(*Canonical))
				{
					OutProperty = *Found;
					return true;
				}
			}

			// Domain-specific pin aliases. UE 5.8 has no MP_Extinction enum value:
			// on MD_Volume materials the volume "Extinction" pin is MP_SubsurfaceColor
			// and "Albedo" is MP_BaseColor (FMaterialAttributeDefinitionMap overrides).
			if (Material && Material->MaterialDomain == MD_Volume)
			{
				if (Compact == TEXT("extinction"))
				{
					OutProperty = MP_SubsurfaceColor;
					return true;
				}
				if (Compact == TEXT("albedo"))
				{
					OutProperty = MP_BaseColor;
					return true;
				}
			}

			// Generic fallback: accept any other EMaterialProperty enum name when the
			// target material reports the property as active for its domain (covers
			// PostProcess/DeferredDecal/UI/etc. without another hardcoded table).
			if (Material)
			{
				if (const UEnum* PropertyEnum = StaticEnum<EMaterialProperty>())
				{
					for (int32 Index = 0; Index < PropertyEnum->NumEnums(); ++Index)
					{
						const int64 Value = PropertyEnum->GetValueByIndex(Index);
						if (Value == MP_MAX || Value == MP_MaterialAttributes || Value == MP_CustomOutput)
						{
							continue;
						}
						FString EnumName = PropertyEnum->GetNameStringByIndex(Index);
						EnumName.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
						if (EnumName.ToLower().Replace(TEXT("_"), TEXT("")) == Compact
							&& Material->IsPropertyActiveInEditor(static_cast<EMaterialProperty>(Value)))
						{
							OutProperty = static_cast<EMaterialProperty>(Value);
							return true;
						}
					}
				}
			}
			return false;
		}

		ALandscape* ResolveLandscape(FSololmcpEditorServices& Services, const FString& LandscapeId, FString& OutError)
		{
			AActor* Actor = Services.FindActorByLabelOrName(LandscapeId, OutError);
			if (!Actor)
			{
				return nullptr;
			}

			if (ALandscape* Landscape = Cast<ALandscape>(Actor))
			{
				return Landscape;
			}

			if (ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Actor))
			{
				return Proxy->GetLandscapeActor();
			}

			// UE5 may spawn LandscapePlaceholder when Landscape has no heightmap data
			if (Actor->GetClass()->GetName().Contains(TEXT("LandscapePlaceholder")))
			{
				OutError = TEXT("Actor is a LandscapePlaceholder (no heightmap data). Use landscape_import or landscape_create_tile_with_data to initialize the landscape.");
				return nullptr;
			}

			OutError = TEXT("Actor is not a landscape.");
			return nullptr;
		}

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		TSharedRef<FJsonObject> LandscapeLayersToJson(ALandscape* Landscape)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Layers;

			if (Landscape)
			{
				for (ULandscapeEditLayerBase* Layer : Landscape->GetEditLayers())
				{
					if (!Layer)
					{
						continue;
					}
					TSharedRef<FJsonObject> LayerJson = MakeShared<FJsonObject>();
					LayerJson->SetStringField(TEXT("name"), Layer->GetName().ToString());
					LayerJson->SetStringField(TEXT("guid"), Layer->GetGuid().ToString());
					LayerJson->SetBoolField(TEXT("visible"), Layer->IsVisible());
					LayerJson->SetBoolField(TEXT("locked"), Layer->IsLocked());
					LayerJson->SetBoolField(TEXT("editing"), Layer->GetGuid() == Landscape->GetEditingLayer());
					LayerJson->SetNumberField(TEXT("heightAlpha"), Layer->GetAlphaForTargetType(ELandscapeToolTargetType::Heightmap));
					LayerJson->SetNumberField(TEXT("weightAlpha"), Layer->GetAlphaForTargetType(ELandscapeToolTargetType::Weightmap));
					Layers.Add(MakeShared<FJsonValueObject>(LayerJson));
				}
			}

			Result->SetArrayField(TEXT("layers"), Layers);
			Result->SetNumberField(TEXT("count"), Layers.Num());
			return Result;
		}

		ULandscapeEditLayerBase* FindLandscapeEditLayer(ALandscape* Landscape, const FString& LayerId, int32* OutLayerIndex = nullptr)
		{
			if (!Landscape)
			{
				return nullptr;
			}

			const TArray<ULandscapeEditLayerBase*>& Layers = Landscape->GetEditLayers();
			FGuid ParsedGuid;
			const bool bHasGuid = FGuid::Parse(LayerId, ParsedGuid);
			for (int32 Index = 0; Index < Layers.Num(); ++Index)
			{
				ULandscapeEditLayerBase* Layer = Layers[Index];
				if (!Layer)
				{
					continue;
				}

				if (Layer->GetName().ToString() == LayerId || (bHasGuid && Layer->GetGuid() == ParsedGuid))
				{
					if (OutLayerIndex)
					{
						*OutLayerIndex = Index;
					}
					return Layer;
				}
			}

			return nullptr;
		}
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

		ULandscapeLayerInfoObject* FindLandscapePaintLayerInfo(ALandscape* Landscape, const FString& LayerNameOrPath)
		{
			if (!Landscape)
			{
				return nullptr;
			}

			ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
			if (!LandscapeInfo)
			{
				return nullptr;
			}

			auto FindInCurrentLayerSettings = [&]() -> ULandscapeLayerInfoObject*
			{
				for (const FLandscapeInfoLayerSettings& LayerSettings : LandscapeInfo->Layers)
				{
					if (ULandscapeLayerInfoObject* LayerInfo = LayerSettings.LayerInfoObj.Get())
					{
						if (SOMOLMCP_LANDSCAPE_LAYER_NAME(LayerInfo).ToString() == LayerNameOrPath || LayerInfo->GetPathName() == LayerNameOrPath)
						{
							return LayerInfo;
						}
					}
				}
				return nullptr;
			};

			if (ULandscapeLayerInfoObject* BoundLayerInfo = FindInCurrentLayerSettings())
			{
				return BoundLayerInfo;
			}

			LandscapeInfo->UpdateLayerInfoMap(Landscape, false);
			return FindInCurrentLayerSettings();
		}

		TSharedRef<FJsonObject> LandscapePaintLayersToJson(ALandscape* Landscape)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> LayersJson;
			if (Landscape)
			{
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (LandscapeInfo)
				{
					auto AppendCurrentLayerSettings = [&]()
					{
						for (const FLandscapeInfoLayerSettings& LayerSettings : LandscapeInfo->Layers)
						{
							ULandscapeLayerInfoObject* LayerInfo = LayerSettings.LayerInfoObj.Get();
							if (!LayerInfo)
							{
								continue;
							}

							TSharedRef<FJsonObject> LayerObject = MakeShared<FJsonObject>();
							LayerObject->SetStringField(TEXT("name"), SOMOLMCP_LANDSCAPE_LAYER_NAME(LayerInfo).ToString());
							LayerObject->SetStringField(TEXT("assetPath"), LayerInfo->GetPathName());
							LayerObject->SetBoolField(TEXT("noWeightBlend"), SOMOLMCP_LANDSCAPE_LAYER_NO_WEIGHT(LayerInfo));
							LayerObject->SetStringField(TEXT("layerName"), LayerSettings.GetLayerName().ToString());
							LayersJson.Add(MakeShared<FJsonValueObject>(LayerObject));
						}
					};

					AppendCurrentLayerSettings();
					if (LayersJson.Num() == 0)
					{
						LandscapeInfo->UpdateLayerInfoMap(Landscape, false);
						AppendCurrentLayerSettings();
					}
				}
			}
			Result->SetArrayField(TEXT("layers"), LayersJson);
			Result->SetNumberField(TEXT("count"), LayersJson.Num());
			return Result;
		}

		struct FLandscapeClipboardEntry
		{
			FString LandscapePath;
			int32 Width = 0;
			int32 Height = 0;
			TArray<uint16> HeightData;
		};

		FLandscapeClipboardEntry GLandscapeClipboard;

		bool GetLandscapeBoundsFromArguments(ALandscape* Landscape, const TSharedRef<FJsonObject>& Arguments, int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY, FString& OutError)
		{
			if (!Landscape)
			{
				OutError = TEXT("Landscape is null.");
				return false;
			}

			const FIntRect Bounds = Landscape->GetBoundingRect();
			OutMinX = Bounds.Min.X;
			OutMinY = Bounds.Min.Y;
			OutMaxX = Bounds.Max.X;
			OutMaxY = Bounds.Max.Y;
			if (Arguments->HasTypedField<EJson::Number>(TEXT("min_x"))) { OutMinX = Arguments->GetIntegerField(TEXT("min_x")); }
			if (Arguments->HasTypedField<EJson::Number>(TEXT("min_y"))) { OutMinY = Arguments->GetIntegerField(TEXT("min_y")); }
			if (Arguments->HasTypedField<EJson::Number>(TEXT("max_x"))) { OutMaxX = Arguments->GetIntegerField(TEXT("max_x")); }
			if (Arguments->HasTypedField<EJson::Number>(TEXT("max_y"))) { OutMaxY = Arguments->GetIntegerField(TEXT("max_y")); }
			OutMinX = FMath::Clamp(OutMinX, Bounds.Min.X, Bounds.Max.X);
			OutMinY = FMath::Clamp(OutMinY, Bounds.Min.Y, Bounds.Max.Y);
			OutMaxX = FMath::Clamp(OutMaxX, Bounds.Min.X, Bounds.Max.X);
			OutMaxY = FMath::Clamp(OutMaxY, Bounds.Min.Y, Bounds.Max.Y);
			if (OutMinX > OutMaxX || OutMinY > OutMaxY)
			{
				OutError = TEXT("Landscape region bounds are invalid.");
				return false;
			}
			return true;
		}

		bool ValidateLandscapeReadbackRegion(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY, FString& OutError)
		{
			const int64 Width = static_cast<int64>(MaxX) - static_cast<int64>(MinX) + 1;
			const int64 Height = static_cast<int64>(MaxY) - static_cast<int64>(MinY) + 1;
			const int64 CellCount = Width * Height;
			constexpr int64 MaxReadbackCells = 512LL * 512LL;
			if (Width <= 0 || Height <= 0)
			{
				OutError = TEXT("Landscape readback region bounds are invalid.");
				return false;
			}
			if (CellCount > MaxReadbackCells)
			{
				OutError = FString::Printf(
					TEXT("Landscape readback region is too large (%lld cells). Pass min_x/min_y/max_x/max_y for a region <= %lld cells."),
					CellCount,
					MaxReadbackCells);
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> LandscapeRegionResultToJson(ALandscape* Landscape, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("landscape"), Landscape ? Landscape->GetPathName() : FString());
			Result->SetNumberField(TEXT("minX"), MinX);
			Result->SetNumberField(TEXT("minY"), MinY);
			Result->SetNumberField(TEXT("maxX"), MaxX);
			Result->SetNumberField(TEXT("maxY"), MaxY);
			Result->SetNumberField(TEXT("width"), MaxX - MinX + 1);
			Result->SetNumberField(TEXT("height"), MaxY - MinY + 1);
			return Result;
		}

		bool ApplyLandscapeHeightBrush(
			ALandscape* Landscape,
			const FVector& WorldCenter,
			double Radius,
			TFunctionRef<uint16(int32, int32, uint16, float)> ApplySample,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			if (!Landscape)
			{
				OutError = TEXT("Landscape is null.");
				return false;
			}

			if (Radius <= 0.0)
			{
				OutError = TEXT("radius must be greater than zero.");
				return false;
			}

			ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
			if (!LandscapeInfo)
			{
				OutError = TEXT("Landscape info is unavailable.");
				return false;
			}

			const FVector LocalCenter = Landscape->GetTransform().InverseTransformPosition(WorldCenter);
			const int32 CenterX = FMath::RoundToInt(LocalCenter.X);
			const int32 CenterY = FMath::RoundToInt(LocalCenter.Y);
			const int32 IntRadius = FMath::Max(1, FMath::CeilToInt(Radius));
			const FIntRect Bounds = Landscape->GetBoundingRect();
			const int32 MinX = FMath::Clamp(CenterX - IntRadius, Bounds.Min.X, Bounds.Max.X);
			const int32 MinY = FMath::Clamp(CenterY - IntRadius, Bounds.Min.Y, Bounds.Max.Y);
			const int32 MaxX = FMath::Clamp(CenterX + IntRadius, Bounds.Min.X, Bounds.Max.X);
			const int32 MaxY = FMath::Clamp(CenterY + IntRadius, Bounds.Min.Y, Bounds.Max.Y);
			if (MinX > MaxX || MinY > MaxY)
			{
				OutError = TEXT("Computed brush bounds are invalid.");
				return false;
			}

			const int32 Width = MaxX - MinX + 1;
			const int32 Height = MaxY - MinY + 1;
			TArray<uint16> HeightData;
			HeightData.SetNumZeroed(Width * Height);

			FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
			LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0);

			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					const int32 GridX = MinX + X;
					const int32 GridY = MinY + Y;
					const double Distance = FVector2D::Distance(FVector2D(GridX, GridY), FVector2D(CenterX, CenterY));
					if (Distance > Radius)
					{
						continue;
					}

					const float Falloff = 1.0f - static_cast<float>(Distance / Radius);
					uint16& Sample = HeightData[Y * Width + X];
					Sample = ApplySample(GridX, GridY, Sample, Falloff);
				}
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeBrushMutation", "SOMOLMCP Landscape Brush Mutation"));
			Landscape->Modify();
			LandscapeEdit.SetHeightData(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0, true);
			OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
			OutSummary = TEXT("Applied landscape brush mutation.");
			return true;
		}

		// =====================================================================
		// BR-01..BR-04 shared BrushKernel stroke envelope for the four native
		// landscape brush writers (landscape_sculpt_brush, landscape_smooth_brush,
		// landscape_flatten_brush, landscape_noise_brush). The envelope mirrors the
		// shared brush_profile_* schema registered by RegisterBrushKernelTools
		// (radius/strength/spacing/falloff/pressure curves/symmetry/mask) and emits
		// writer receipts that brush_stroke_commit can gate on (receipt_id, status,
		// target_path, mutation_applied, readback_verified).
		// =====================================================================

		struct FSololmcpBrushStrokeProfile
		{
			double Radius = 0.0;
			double Strength = 1.0;
			double Pressure = 1.0;
			double Spacing = 0.25;
			FString Falloff = TEXT("smooth");
			TArray<double> FalloffCurve;
			TArray<double> PressureCurve;
			bool bSymmetryX = false;
			bool bSymmetryY = false;
			bool bSymmetryZ = false;
			FString MaskAssetPath;
			int32 Seed = 0;
			FString StrokeId;
			int32 HeightFilterMin = -1;
			int32 HeightFilterMax = -1;

			bool HasHeightFilter() const { return HeightFilterMin >= 0 && HeightFilterMax >= 0; }
		};

		FCriticalSection& SololmcpBrushTargetLockMutex()
		{
			static FCriticalSection Mutex;
			return Mutex;
		}

		TMap<FString, FString>& SololmcpBrushTargetLockOwners()
		{
			static TMap<FString, FString> Owners;
			return Owners;
		}

		bool AcquireSololmcpBrushTargetLock(const FString& TargetPath, const FString& Owner, FString& OutBlockingOwner)
		{
			FScopeLock Lock(&SololmcpBrushTargetLockMutex());
			TMap<FString, FString>& Owners = SololmcpBrushTargetLockOwners();
			if (const FString* Holder = Owners.Find(TargetPath))
			{
				if (*Holder != Owner)
				{
					OutBlockingOwner = *Holder;
					return false;
				}
				return true;
			}
			Owners.Add(TargetPath, Owner);
			return true;
		}

		void ReleaseSololmcpBrushTargetLock(const FString& TargetPath, const FString& Owner)
		{
			FScopeLock Lock(&SololmcpBrushTargetLockMutex());
			TMap<FString, FString>& Owners = SololmcpBrushTargetLockOwners();
			if (const FString* Holder = Owners.Find(TargetPath))
			{
				if (*Holder == Owner)
				{
					Owners.Remove(TargetPath);
				}
			}
		}

		struct FSololmcpBrushTargetLockGuard
		{
			FString TargetPath;
			FString Owner;
			FString BlockingOwner;
			bool bAcquired = false;

			FSololmcpBrushTargetLockGuard(const FString& InTargetPath, const FString& InOwner)
				: TargetPath(InTargetPath)
				, Owner(InOwner)
			{
				bAcquired = AcquireSololmcpBrushTargetLock(TargetPath, Owner, BlockingOwner);
			}

			~FSololmcpBrushTargetLockGuard()
			{
				if (bAcquired)
				{
					ReleaseSololmcpBrushTargetLock(TargetPath, Owner);
				}
			}
		};

		bool SololmcpBrushStrokeFail(
			TSharedRef<FJsonObject>& Out,
			FString& OutError,
			const FString& ReasonCode,
			const FString& Message,
			const FString& Status = TEXT("failed"))
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("status"), Status);
			Out->SetStringField(TEXT("error_code"), ReasonCode);
			Out->SetStringField(TEXT("reason_code"), ReasonCode);
			Out->SetStringField(TEXT("message"), Message);
			Out->SetBoolField(TEXT("mutation_applied"), false);
			Out->SetBoolField(TEXT("readback_verified"), false);
			OutError = Message;
			return false;
		}

		bool ReadSololmcpBrushCurve(
			const TSharedRef<FJsonObject>& Arguments,
			const TCHAR* Field,
			TArray<double>& OutCurve,
			TSharedRef<FJsonObject>& Out,
			FString& OutError,
			const FString& ReasonCode)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Arguments->TryGetArrayField(Field, Values) || !Values)
			{
				return true;
			}
			OutCurve.Reset();
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				double Sample = 0.0;
				if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Sample)
					|| !FMath::IsFinite(Sample) || Sample < 0.0 || Sample > 1.0)
				{
					return SololmcpBrushStrokeFail(Out, OutError, ReasonCode,
						FString::Printf(TEXT("%s samples must be finite numbers in [0, 1]."), Field));
				}
				OutCurve.Add(Sample);
			}
			if (OutCurve.Num() < 2 || OutCurve.Num() > 64)
			{
				return SololmcpBrushStrokeFail(Out, OutError, ReasonCode,
					FString::Printf(TEXT("%s requires between 2 and 64 samples."), Field));
			}
			return true;
		}

		bool ParseSololmcpBrushStrokeEnvelope(
			const TSharedRef<FJsonObject>& Arguments,
			FSololmcpBrushStrokeProfile& Profile,
			TSharedRef<FJsonObject>& Out,
			FString& OutError)
		{
			if (!FMath::IsFinite(Profile.Radius) || Profile.Radius <= 0.0)
			{
				return SololmcpBrushStrokeFail(Out, OutError, TEXT("invalid_brush_radius"),
					TEXT("radius must be a finite value greater than zero."));
			}
			if (!FMath::IsFinite(Profile.Strength) || Profile.Strength < 0.0)
			{
				return SololmcpBrushStrokeFail(Out, OutError, TEXT("invalid_brush_strength"),
					TEXT("strength must be a finite value greater than or equal to zero."));
			}
			if (Arguments->HasTypedField<EJson::Number>(TEXT("pressure")))
			{
				Profile.Pressure = Arguments->GetNumberField(TEXT("pressure"));
				if (!FMath::IsFinite(Profile.Pressure) || Profile.Pressure < 0.0 || Profile.Pressure > 1.0)
				{
					return SololmcpBrushStrokeFail(Out, OutError, TEXT("invalid_brush_pressure"),
						TEXT("pressure must be a finite value in [0, 1]."));
				}
			}
			if (Arguments->HasTypedField<EJson::Number>(TEXT("spacing")))
			{
				Profile.Spacing = Arguments->GetNumberField(TEXT("spacing"));
				if (!FMath::IsFinite(Profile.Spacing) || Profile.Spacing < 0.001 || Profile.Spacing > 10.0)
				{
					return SololmcpBrushStrokeFail(Out, OutError, TEXT("invalid_brush_spacing"),
						TEXT("spacing must be a finite value in [0.001, 10]."));
				}
			}
			FString Falloff;
			if (Arguments->TryGetStringField(TEXT("falloff"), Falloff) && !Falloff.TrimStartAndEnd().IsEmpty())
			{
				static const FString AllowedFalloff[] = { TEXT("linear"), TEXT("smooth"), TEXT("spherical"), TEXT("tip"), TEXT("custom") };
				bool bAllowed = false;
				for (const FString& Candidate : AllowedFalloff)
				{
					if (Falloff == Candidate)
					{
						bAllowed = true;
						break;
					}
				}
				if (!bAllowed)
				{
					return SololmcpBrushStrokeFail(Out, OutError, TEXT("invalid_brush_falloff"),
						TEXT("falloff must be one of linear/smooth/spherical/tip/custom."));
				}
				Profile.Falloff = Falloff;
			}
			if (!ReadSololmcpBrushCurve(Arguments, TEXT("falloff_curve"), Profile.FalloffCurve, Out, OutError, TEXT("invalid_falloff_curve")))
			{
				return false;
			}
			if (Profile.FalloffCurve.Num() > 0)
			{
				Profile.Falloff = TEXT("custom");
			}
			if (!ReadSololmcpBrushCurve(Arguments, TEXT("pressure_curve"), Profile.PressureCurve, Out, OutError, TEXT("invalid_pressure_curve")))
			{
				return false;
			}
			if (Arguments->HasTypedField<EJson::Boolean>(TEXT("symmetry_x"))) Profile.bSymmetryX = Arguments->GetBoolField(TEXT("symmetry_x"));
			if (Arguments->HasTypedField<EJson::Boolean>(TEXT("symmetry_y"))) Profile.bSymmetryY = Arguments->GetBoolField(TEXT("symmetry_y"));
			if (Arguments->HasTypedField<EJson::Boolean>(TEXT("symmetry_z"))) Profile.bSymmetryZ = Arguments->GetBoolField(TEXT("symmetry_z"));
			FString MaskPath;
			if (Arguments->TryGetStringField(TEXT("mask_asset_path"), MaskPath))
			{
				MaskPath = MaskPath.TrimStartAndEnd();
				if (!MaskPath.IsEmpty() && !MaskPath.StartsWith(TEXT("/Game/")))
				{
					return SololmcpBrushStrokeFail(Out, OutError, TEXT("invalid_mask_asset_path"),
						TEXT("mask_asset_path must be empty or start with /Game/."));
				}
				Profile.MaskAssetPath = MaskPath;
			}
			FString StrokeId;
			if (Arguments->TryGetStringField(TEXT("stroke_id"), StrokeId))
			{
				Profile.StrokeId = StrokeId.TrimStartAndEnd();
			}
			const bool bHasHeightMin = Arguments->HasTypedField<EJson::Number>(TEXT("height_filter_min"));
			const bool bHasHeightMax = Arguments->HasTypedField<EJson::Number>(TEXT("height_filter_max"));
			if (bHasHeightMin != bHasHeightMax)
			{
				return SololmcpBrushStrokeFail(Out, OutError, TEXT("invalid_height_filter"),
					TEXT("height_filter_min and height_filter_max must be provided together."));
			}
			if (bHasHeightMin && bHasHeightMax)
			{
				const int32 FilterMin = Arguments->GetIntegerField(TEXT("height_filter_min"));
				const int32 FilterMax = Arguments->GetIntegerField(TEXT("height_filter_max"));
				if (FilterMin < 0 || FilterMax > TNumericLimits<uint16>::Max() || FilterMin > FilterMax)
				{
					return SololmcpBrushStrokeFail(Out, OutError, TEXT("invalid_height_filter"),
						TEXT("height_filter_min/max must be raw uint16 heights with min <= max."));
				}
				Profile.HeightFilterMin = FilterMin;
				Profile.HeightFilterMax = FilterMax;
			}
			return true;
		}

		float EvaluateSololmcpBrushFalloff(const FSololmcpBrushStrokeProfile& Profile, double NormalizedDistance)
		{
			const double T = FMath::Clamp(NormalizedDistance, 0.0, 1.0);
			if (Profile.Falloff == TEXT("custom") && Profile.FalloffCurve.Num() >= 2)
			{
				const double Position = T * (Profile.FalloffCurve.Num() - 1);
				const int32 Index = FMath::Clamp(static_cast<int32>(Position), 0, Profile.FalloffCurve.Num() - 2);
				return static_cast<float>(FMath::Lerp(Profile.FalloffCurve[Index], Profile.FalloffCurve[Index + 1], Position - Index));
			}
			if (Profile.Falloff == TEXT("linear"))
			{
				return static_cast<float>(1.0 - T);
			}
			if (Profile.Falloff == TEXT("spherical"))
			{
				return static_cast<float>(FMath::Sqrt(FMath::Max(0.0, 1.0 - T * T)));
			}
			if (Profile.Falloff == TEXT("tip"))
			{
				return static_cast<float>(FMath::Pow(1.0 - T, 3.0));
			}
			const double Smooth = T * T * (3.0 - 2.0 * T);
			return static_cast<float>(1.0 - Smooth);
		}

		double EvaluateSololmcpBrushPressure(const FSololmcpBrushStrokeProfile& Profile)
		{
			double Pressure = FMath::Clamp(Profile.Pressure, 0.0, 1.0);
			if (Profile.PressureCurve.Num() >= 2)
			{
				const double Position = Pressure * (Profile.PressureCurve.Num() - 1);
				const int32 Index = FMath::Clamp(static_cast<int32>(Position), 0, Profile.PressureCurve.Num() - 2);
				Pressure = FMath::Lerp(Profile.PressureCurve[Index], Profile.PressureCurve[Index + 1], Position - Index);
			}
			return Pressure;
		}

		bool ResolveSololmcpBrushMask(
			const FSololmcpBrushStrokeProfile& Profile,
			TArray<uint8>& OutMaskBytes,
			int32& OutSizeX,
			int32& OutSizeY,
			TSharedRef<FJsonObject>& Out,
			FString& OutError)
		{
			OutSizeX = 0;
			OutSizeY = 0;
			if (Profile.MaskAssetPath.IsEmpty())
			{
				return true;
			}
			UTexture2D* Mask = LoadObject<UTexture2D>(nullptr, *Profile.MaskAssetPath);
			if (!Mask)
			{
				return SololmcpBrushStrokeFail(Out, OutError, TEXT("brush_mask_not_found"),
					TEXT("mask_asset_path does not resolve to a Texture2D asset."));
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
			const FTexturePlatformData* PlatformData = Mask->GetPlatformData();
#else
			const FTexturePlatformData* PlatformData = Mask->PlatformData;
#endif
			if (!PlatformData || PlatformData->Mips.Num() == 0 || PlatformData->SizeX <= 0 || PlatformData->SizeY <= 0)
			{
				return SololmcpBrushStrokeFail(Out, OutError, TEXT("brush_mask_unavailable"),
					TEXT("The bound mask texture has no readable platform data."));
			}
			if (PlatformData->PixelFormat != PF_B8G8R8A8)
			{
				return SololmcpBrushStrokeFail(Out, OutError, TEXT("brush_mask_unsupported_format"),
					TEXT("Only PF_B8G8R8A8 mask textures are supported."));
			}
			const FTexture2DMipMap& Mip = PlatformData->Mips[0];
			const int64 ByteCount = static_cast<int64>(PlatformData->SizeX) * static_cast<int64>(PlatformData->SizeY) * 4;
			if (Mip.BulkData.GetBulkDataSize() < ByteCount)
			{
				return SololmcpBrushStrokeFail(Out, OutError, TEXT("brush_mask_unavailable"),
					TEXT("The bound mask texture mip does not contain enough bulk data."));
			}
			const uint8* Bytes = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
			if (!Bytes)
			{
				return SololmcpBrushStrokeFail(Out, OutError, TEXT("brush_mask_unavailable"),
					TEXT("The bound mask texture bulk data could not be locked."));
			}
			OutMaskBytes.SetNumUninitialized(static_cast<int32>(ByteCount));
			FMemory::Memcpy(OutMaskBytes.GetData(), Bytes, static_cast<SIZE_T>(ByteCount));
			Mip.BulkData.Unlock();
			OutSizeX = PlatformData->SizeX;
			OutSizeY = PlatformData->SizeY;
			return true;
		}

		float SampleSololmcpBrushMask(const TArray<uint8>& MaskBytes, int32 SizeX, int32 SizeY, double U, double V)
		{
			if (SizeX <= 0 || SizeY <= 0 || MaskBytes.Num() < SizeX * SizeY * 4)
			{
				return 1.0f;
			}
			const int32 X = FMath::Clamp(FMath::FloorToInt(U * SizeX), 0, SizeX - 1);
			const int32 Y = FMath::Clamp(FMath::FloorToInt(V * SizeY), 0, SizeY - 1);
			const uint8* Pixel = &MaskBytes[(Y * SizeX + X) * 4];
			return static_cast<float>(Pixel[0] + Pixel[1] + Pixel[2]) / (3.0f * 255.0f);
		}

		struct FSololmcpBrushHeightSnapshot
		{
			int32 MinX = 0;
			int32 MinY = 0;
			int32 MaxX = 0;
			int32 MaxY = 0;
			TArray<uint16> HeightData;
			FString StateHash;

			bool IsValid() const { return HeightData.Num() > 0; }
		};

		bool CaptureLandscapeBrushHeightSnapshot(
			ALandscape* Landscape,
			ULandscapeInfo* LandscapeInfo,
			int32 MinX,
			int32 MinY,
			int32 MaxX,
			int32 MaxY,
			FSololmcpBrushHeightSnapshot& OutSnapshot,
			FString& OutError)
		{
			if (!Landscape || !LandscapeInfo || MinX > MaxX || MinY > MaxY)
			{
				OutError = TEXT("Landscape brush snapshot bounds are invalid.");
				return false;
			}
			OutSnapshot.MinX = MinX;
			OutSnapshot.MinY = MinY;
			OutSnapshot.MaxX = MaxX;
			OutSnapshot.MaxY = MaxY;
			OutSnapshot.HeightData.SetNumZeroed((MaxX - MinX + 1) * (MaxY - MinY + 1));
			FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
			LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, OutSnapshot.HeightData.GetData(), 0);
			OutSnapshot.StateHash = FString::Printf(TEXT("%08x"),
				FCrc::MemCrc32(OutSnapshot.HeightData.GetData(), OutSnapshot.HeightData.Num() * sizeof(uint16)));
			return true;
		}

		bool RestoreLandscapeBrushHeightSnapshot(
			ALandscape* Landscape,
			ULandscapeInfo* LandscapeInfo,
			const FSololmcpBrushHeightSnapshot& Snapshot)
		{
			if (!Landscape || !LandscapeInfo || !Snapshot.IsValid())
			{
				return false;
			}
			Landscape->Modify();
			FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
			LandscapeEdit.SetHeightData(Snapshot.MinX, Snapshot.MinY, Snapshot.MaxX, Snapshot.MaxY,
				Snapshot.HeightData.GetData(), 0, true);
			TArray<uint16> Readback;
			Readback.SetNumZeroed(Snapshot.HeightData.Num());
			LandscapeEdit.GetHeightDataFast(Snapshot.MinX, Snapshot.MinY, Snapshot.MaxX, Snapshot.MaxY, Readback.GetData(), 0);
			return Readback == Snapshot.HeightData;
		}

		TSharedRef<FJsonObject> BrushStrokeEnvelopeObjectSchema(
			const TMap<FString, TSharedRef<FJsonObject>>& ExtraProperties,
			const TArray<FString>& ExtraRequired)
		{
			const TSharedRef<FJsonObject> CurveSchema = FSololmcpSchemaBuilder::Array(
				FSololmcpSchemaBuilder::Number(TEXT("Normalized sample."), 0.0, 1.0), TEXT("Normalized curve samples."), 2, 64);
			TMap<FString, TSharedRef<FJsonObject>> Properties = {
				{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor label, name or path."))},
				{TEXT("center"), VectorSchema(TEXT("World-space brush center."))},
				{TEXT("radius"), FSololmcpSchemaBuilder::Number(TEXT("Brush radius in world units (cm)."))},
				{TEXT("strength"), FSololmcpSchemaBuilder::Number(TEXT("Tool-specific brush strength."))},
				{TEXT("pressure"), FSololmcpSchemaBuilder::Number(TEXT("Normalized pressure multiplier."), 0.0, 1.0)},
				{TEXT("spacing"), FSololmcpSchemaBuilder::Number(TEXT("Normalized stamp spacing."), 0.001, 10.0)},
				{TEXT("falloff"), FSololmcpSchemaBuilder::String(TEXT("linear/smooth/spherical/tip/custom"), {TEXT("linear"), TEXT("smooth"), TEXT("spherical"), TEXT("tip"), TEXT("custom")})},
				{TEXT("falloff_curve"), CurveSchema},
				{TEXT("pressure_curve"), CurveSchema},
				{TEXT("symmetry_x"), FSololmcpSchemaBuilder::Boolean(TEXT("Mirror the stroke across the local X axis."))},
				{TEXT("symmetry_y"), FSololmcpSchemaBuilder::Boolean(TEXT("Mirror the stroke across the local Y axis."))},
				{TEXT("symmetry_z"), FSololmcpSchemaBuilder::Boolean(TEXT("Reserved Z symmetry flag (echoed in the receipt)."))},
				{TEXT("mask_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional /Game Texture2D mask multiplied into the envelope."))},
				{TEXT("stroke_id"), FSololmcpSchemaBuilder::String(TEXT("Optional brush_stroke_begin stroke id linkage."))},
				{TEXT("height_filter_min"), FSololmcpSchemaBuilder::Integer(TEXT("Skip samples below this raw uint16 height."))},
				{TEXT("height_filter_max"), FSololmcpSchemaBuilder::Integer(TEXT("Skip samples above this raw uint16 height."))}
			};
			for (const TPair<FString, TSharedRef<FJsonObject>>& Extra : ExtraProperties)
			{
				Properties.Add(Extra.Key, Extra.Value);
			}
			TArray<FString> Required = {TEXT("landscape"), TEXT("center"), TEXT("radius")};
			Required.Append(ExtraRequired);
			return FSololmcpSchemaBuilder::Object(Properties, Required);
		}

		bool ApplyLandscapeBrushStrokeEnvelope(
			ALandscape* Landscape,
			const FVector& WorldCenter,
			const FSololmcpBrushStrokeProfile& Profile,
			const FString& ToolName,
			const TCHAR* OperationName,
			TFunctionRef<uint16(int32 GridX, int32 GridY, uint16 Current, float Envelope)> ApplySample,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			if (!Landscape)
			{
				return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("landscape_unavailable"),
					TEXT("Landscape is null."));
			}
			const FString TargetPath = Landscape->GetPathName();
			const FString LockOwner = FString::Printf(TEXT("%s_%s"), OperationName,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
			FSololmcpBrushTargetLockGuard TargetLock(TargetPath, LockOwner);
			if (!TargetLock.bAcquired)
			{
				const bool bResult = SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("brush_target_locked"),
					FString::Printf(TEXT("Target %s is locked by another brush writer."), *TargetPath), TEXT("blocked"));
				OutStructured->SetStringField(TEXT("blocking_lock_owner"), TargetLock.BlockingOwner);
				return bResult;
			}
			ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
			if (!LandscapeInfo)
			{
				return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("landscape_info_unavailable"),
					TEXT("Landscape info is unavailable."));
			}
			TArray<uint8> MaskBytes;
			int32 MaskSizeX = 0;
			int32 MaskSizeY = 0;
			if (!ResolveSololmcpBrushMask(Profile, MaskBytes, MaskSizeX, MaskSizeY, OutStructured, OutError))
			{
				return false;
			}
			const FVector LocalCenter = Landscape->GetTransform().InverseTransformPosition(WorldCenter);
			TArray<FVector2D> Centers;
			Centers.Add(FVector2D(LocalCenter.X, LocalCenter.Y));
			if (Profile.bSymmetryX)
			{
				Centers.Add(FVector2D(-LocalCenter.X, LocalCenter.Y));
			}
			if (Profile.bSymmetryY)
			{
				Centers.Add(FVector2D(LocalCenter.X, -LocalCenter.Y));
			}
			if (Profile.bSymmetryX && Profile.bSymmetryY)
			{
				Centers.Add(FVector2D(-LocalCenter.X, -LocalCenter.Y));
			}
			const int32 IntRadius = FMath::Max(1, FMath::CeilToInt(Profile.Radius));
			const FIntRect LandscapeBounds = Landscape->GetBoundingRect();
			int32 MinX = TNumericLimits<int32>::Max();
			int32 MinY = TNumericLimits<int32>::Max();
			int32 MaxX = TNumericLimits<int32>::Lowest();
			int32 MaxY = TNumericLimits<int32>::Lowest();
			for (const FVector2D& Center : Centers)
			{
				const int32 CenterX = FMath::RoundToInt(Center.X);
				const int32 CenterY = FMath::RoundToInt(Center.Y);
				MinX = FMath::Min(MinX, CenterX - IntRadius);
				MinY = FMath::Min(MinY, CenterY - IntRadius);
				MaxX = FMath::Max(MaxX, CenterX + IntRadius);
				MaxY = FMath::Max(MaxY, CenterY + IntRadius);
			}
			MinX = FMath::Clamp(MinX, LandscapeBounds.Min.X, LandscapeBounds.Max.X);
			MinY = FMath::Clamp(MinY, LandscapeBounds.Min.Y, LandscapeBounds.Max.Y);
			MaxX = FMath::Clamp(MaxX, LandscapeBounds.Min.X, LandscapeBounds.Max.X);
			MaxY = FMath::Clamp(MaxY, LandscapeBounds.Min.Y, LandscapeBounds.Max.Y);
			if (MinX > MaxX || MinY > MaxY)
			{
				return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("brush_bounds_invalid"),
					TEXT("Computed brush bounds are invalid; the stroke is outside the landscape."));
			}
			const int32 Width = MaxX - MinX + 1;
			const int32 Height = MaxY - MinY + 1;
			constexpr int64 MaxBrushCells = 4096LL * 4096LL;
			if (static_cast<int64>(Width) * static_cast<int64>(Height) > MaxBrushCells)
			{
				return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("brush_region_too_large"),
					TEXT("The brush radius covers too many landscape cells; reduce radius."));
			}
			FSololmcpBrushHeightSnapshot PreSnapshot;
			if (!CaptureLandscapeBrushHeightSnapshot(Landscape, LandscapeInfo, MinX, MinY, MaxX, MaxY, PreSnapshot, OutError))
			{
				return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("brush_snapshot_failed"), OutError);
			}
			TArray<uint16> NewData;
			NewData.SetNumZeroed(Width * Height);
			{
				FLandscapeEditDataInterface ReadEdit(LandscapeInfo);
				ReadEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, NewData.GetData(), 0);
			}
			const double PressureFactor = EvaluateSololmcpBrushPressure(Profile);
			int32 SamplesAffected = 0;
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					const int32 GridX = MinX + X;
					const int32 GridY = MinY + Y;
					uint16& Sample = NewData[Y * Width + X];
					if (Profile.HasHeightFilter() && (Sample < Profile.HeightFilterMin || Sample > Profile.HeightFilterMax))
					{
						continue;
					}
					double BestEnvelope = 0.0;
					for (const FVector2D& Center : Centers)
					{
						const double Distance = FVector2D::Distance(FVector2D(GridX, GridY), Center);
						if (Distance > Profile.Radius)
						{
							continue;
						}
						BestEnvelope = FMath::Max(BestEnvelope,
							static_cast<double>(EvaluateSololmcpBrushFalloff(Profile, Distance / Profile.Radius)) * PressureFactor);
					}
					if (BestEnvelope <= 0.0)
					{
						continue;
					}
					if (MaskSizeX > 0 && MaskSizeY > 0)
					{
						const double U = Width > 1 ? static_cast<double>(X) / (Width - 1) : 0.0;
						const double V = Height > 1 ? static_cast<double>(Y) / (Height - 1) : 0.0;
						BestEnvelope *= SampleSololmcpBrushMask(MaskBytes, MaskSizeX, MaskSizeY, U, V);
						if (BestEnvelope <= 0.0)
						{
							continue;
						}
					}
					const uint16 Original = Sample;
					Sample = ApplySample(GridX, GridY, Sample, static_cast<float>(BestEnvelope));
					if (Sample != Original)
					{
						++SamplesAffected;
					}
				}
			}
			{
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeBrushKernelStroke", "SOMOLMCP Landscape Brush Kernel Stroke"));
				Landscape->Modify();
				FLandscapeEditDataInterface WriteEdit(LandscapeInfo);
				WriteEdit.SetHeightData(MinX, MinY, MaxX, MaxY, NewData.GetData(), 0, true);
			}
			TArray<uint16> ImmediateReadback;
			ImmediateReadback.SetNumZeroed(NewData.Num());
			{
				FLandscapeEditDataInterface ReadEdit(LandscapeInfo);
				ReadEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, ImmediateReadback.GetData(), 0);
			}
			auto RollbackAndFail = [&](const FString& ReasonCode, const FString& Message) -> bool
			{
				bool bRollbackVerified = false;
				{
					const FScopedTransaction RollbackTransaction(NSLOCTEXT("SOMOLMCP", "LandscapeBrushKernelRollback", "SOMOLMCP Landscape Brush Kernel Rollback"));
					bRollbackVerified = RestoreLandscapeBrushHeightSnapshot(Landscape, LandscapeInfo, PreSnapshot);
				}
				if (bRollbackVerified)
				{
					Landscape->MarkPackageDirty();
					if (ULevel* Level = Landscape->GetLevel())
					{
						FEditorFileUtils::SaveLevel(Level);
					}
				}
				const bool bResult = SololmcpBrushStrokeFail(OutStructured, OutError, ReasonCode, Message);
				OutStructured->SetBoolField(TEXT("rollback_attempted"), true);
				OutStructured->SetBoolField(TEXT("rollback_verified"), bRollbackVerified);
				OutStructured->SetStringField(TEXT("pre_snapshot_hash"), PreSnapshot.StateHash);
				return bResult;
			};
			if (ImmediateReadback != NewData)
			{
				return RollbackAndFail(TEXT("brush_write_readback_mismatch"),
					TEXT("Landscape brush write failed immediate readback; pre-snapshot was restored."));
			}
			Landscape->MarkPackageDirty();
			ULevel* Level = Landscape->GetLevel();
			const FString LevelPackagePath = Level ? Level->GetOutermost()->GetName() : FString();
			// Headless launches start on the transient /Temp/Untitled_1 world (OpenWorld
			// template). It has no on-disk path, and save-current-as cannot persist it
			// (HLOD proxies reference /Temp/... private objects), so SaveLevel always
			// fails there. The brush write above is already real (immediate readback);
			// degrade persist explicitly instead of rolling back a valid stroke, and
			// keep the strict fail-closed rollback for any real /Game level.
			const bool bTransientWorld = LevelPackagePath.StartsWith(TEXT("/Temp/"));
			const bool bSaved = !bTransientWorld && Level && FEditorFileUtils::SaveLevel(Level);
			FString LevelFilename;
			if (Level)
			{
				LevelFilename = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(
					Level->GetOutermost()->GetName(), FPackageName::GetMapPackageExtension()));
			}
			const bool bSaveFileExists = !LevelFilename.IsEmpty() && IFileManager::Get().FileExists(*LevelFilename);
			if ((!bSaved || !bSaveFileExists) && !bTransientWorld)
			{
				return RollbackAndFail(TEXT("brush_save_failed"),
					TEXT("Landscape level save failed after the brush write; pre-snapshot was restored."));
			}
			TArray<uint16> PersistedReadback;
			PersistedReadback.SetNumZeroed(NewData.Num());
			{
				FLandscapeEditDataInterface ReadEdit(LandscapeInfo);
				ReadEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, PersistedReadback.GetData(), 0);
			}
			if (PersistedReadback != NewData)
			{
				return RollbackAndFail(TEXT("brush_save_readback_mismatch"),
					TEXT("Landscape brush write failed post-save readback; pre-snapshot was restored."));
			}
			const FString ReceiptId = FString::Printf(TEXT("landscape_brush_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower());
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetBoolField(TEXT("ok"), true);
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetStringField(TEXT("receipt_id"), ReceiptId);
			Receipt->SetStringField(TEXT("tool"), ToolName);
			Receipt->SetStringField(TEXT("brush_operation"), OperationName);
			Receipt->SetStringField(TEXT("target_path"), TargetPath);
			Receipt->SetBoolField(TEXT("mutation_applied"), true);
			Receipt->SetBoolField(TEXT("readback_verified"), true);
			Receipt->SetBoolField(TEXT("saved"), !bTransientWorld);
			Receipt->SetBoolField(TEXT("save_verified"), !bTransientWorld && bSaveFileExists);
			if (bTransientWorld)
			{
				Receipt->SetStringField(TEXT("persist_reason"), TEXT("transient_world_not_savable"));
			}
			Receipt->SetStringField(TEXT("package_filename"), LevelFilename);
			Receipt->SetBoolField(TEXT("target_lock_acquired"), true);
			Receipt->SetStringField(TEXT("target_lock_owner"), LockOwner);
			TSharedRef<FJsonObject> SnapshotJson = MakeShared<FJsonObject>();
			SnapshotJson->SetStringField(TEXT("state_hash"), PreSnapshot.StateHash);
			SnapshotJson->SetNumberField(TEXT("sample_count"), PreSnapshot.HeightData.Num());
			SnapshotJson->SetNumberField(TEXT("min_x"), PreSnapshot.MinX);
			SnapshotJson->SetNumberField(TEXT("min_y"), PreSnapshot.MinY);
			SnapshotJson->SetNumberField(TEXT("max_x"), PreSnapshot.MaxX);
			SnapshotJson->SetNumberField(TEXT("max_y"), PreSnapshot.MaxY);
			Receipt->SetObjectField(TEXT("pre_snapshot"), SnapshotJson);
			Receipt->SetBoolField(TEXT("rollback_available"), true);
			Receipt->SetNumberField(TEXT("minX"), MinX);
			Receipt->SetNumberField(TEXT("minY"), MinY);
			Receipt->SetNumberField(TEXT("maxX"), MaxX);
			Receipt->SetNumberField(TEXT("maxY"), MaxY);
			Receipt->SetNumberField(TEXT("width"), Width);
			Receipt->SetNumberField(TEXT("height"), Height);
			Receipt->SetNumberField(TEXT("samples_affected"), SamplesAffected);
			TSharedRef<FJsonObject> EnvelopeJson = MakeShared<FJsonObject>();
			EnvelopeJson->SetNumberField(TEXT("radius_cm"), Profile.Radius);
			EnvelopeJson->SetNumberField(TEXT("strength"), Profile.Strength);
			EnvelopeJson->SetNumberField(TEXT("pressure"), Profile.Pressure);
			EnvelopeJson->SetNumberField(TEXT("spacing"), Profile.Spacing);
			EnvelopeJson->SetStringField(TEXT("falloff"), Profile.Falloff);
			EnvelopeJson->SetBoolField(TEXT("symmetry_x"), Profile.bSymmetryX);
			EnvelopeJson->SetBoolField(TEXT("symmetry_y"), Profile.bSymmetryY);
			EnvelopeJson->SetBoolField(TEXT("symmetry_z"), Profile.bSymmetryZ);
			EnvelopeJson->SetStringField(TEXT("mask_asset_path"), Profile.MaskAssetPath);
			EnvelopeJson->SetNumberField(TEXT("seed"), Profile.Seed);
			Receipt->SetObjectField(TEXT("stroke_profile"), EnvelopeJson);
			if (!Profile.StrokeId.IsEmpty())
			{
				Receipt->SetStringField(TEXT("stroke_id"), Profile.StrokeId);
				Receipt->SetBoolField(TEXT("stroke_commit_gate"), true);
			}
			OutStructured = Receipt;
			OutSummary = FString::Printf(TEXT("Applied %s brush stroke to %s (%d samples affected, receipt %s)."),
				OperationName, *TargetPath, SamplesAffected, *ReceiptId);
			return true;
		}

		bool TryGetUInt16Array(const TSharedRef<FJsonObject>& Object, const FString& FieldName, TArray<uint16>& OutArray)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(FieldName, Values) || !Values)
			{
				return false;
			}

			OutArray.Reset();
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				OutArray.Add(static_cast<uint16>(Value->AsNumber()));
			}
			return true;
		}

		bool TryGetUInt8Array(const TSharedRef<FJsonObject>& Object, const FString& FieldName, TArray<uint8>& OutArray)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(FieldName, Values) || !Values)
			{
				return false;
			}

			OutArray.Reset();
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				OutArray.Add(static_cast<uint8>(Value->AsNumber()));
			}
			return true;
		}

		FString NiagaraTypeToString(const FNiagaraTypeDefinition& Type)
		{
			if (Type == FNiagaraTypeDefinition::GetFloatDef()) { return TEXT("float"); }
			if (Type == FNiagaraTypeDefinition::GetIntDef()) { return TEXT("int"); }
			if (Type == FNiagaraTypeDefinition::GetBoolDef()) { return TEXT("bool"); }
			if (Type == FNiagaraTypeDefinition::GetVec2Def()) { return TEXT("vector2"); }
			if (Type == FNiagaraTypeDefinition::GetVec3Def()) { return TEXT("vector"); }
			if (Type == FNiagaraTypeDefinition::GetVec4Def()) { return TEXT("vector4"); }
			if (Type == FNiagaraTypeDefinition::GetColorDef()) { return TEXT("color"); }
			if (Type == FNiagaraTypeDefinition::GetQuatDef()) { return TEXT("quat"); }
			return Type.GetNameText().ToString();
		}

		bool TryParseNiagaraType(const FString& TypeName, FNiagaraTypeDefinition& OutType)
		{
			if (TypeName == TEXT("float"))
			{
				OutType = FNiagaraTypeDefinition::GetFloatDef();
				return true;
			}
			if (TypeName == TEXT("int"))
			{
				OutType = FNiagaraTypeDefinition::GetIntDef();
				return true;
			}
			if (TypeName == TEXT("bool"))
			{
				OutType = FNiagaraTypeDefinition::GetBoolDef();
				return true;
			}
			if (TypeName == TEXT("vector2"))
			{
				OutType = FNiagaraTypeDefinition::GetVec2Def();
				return true;
			}
			if (TypeName == TEXT("vector"))
			{
				OutType = FNiagaraTypeDefinition::GetVec3Def();
				return true;
			}
			if (TypeName == TEXT("vector4"))
			{
				OutType = FNiagaraTypeDefinition::GetVec4Def();
				return true;
			}
			if (TypeName == TEXT("color"))
			{
				OutType = FNiagaraTypeDefinition::GetColorDef();
				return true;
			}
			if (TypeName == TEXT("quat"))
			{
				OutType = FNiagaraTypeDefinition::GetQuatDef();
				return true;
			}
			return false;
		}

		TSharedRef<FJsonObject> NiagaraVariableToJson(const FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Variable)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), Variable.GetName().ToString());
			Result->SetStringField(TEXT("type"), NiagaraTypeToString(Variable.GetType()));

			if (Variable.GetType() == FNiagaraTypeDefinition::GetFloatDef())
			{
				Result->SetNumberField(TEXT("value"), Store.GetParameterValue<float>(Variable));
			}
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetIntDef())
			{
				Result->SetNumberField(TEXT("value"), Store.GetParameterValue<int32>(Variable));
			}
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetBoolDef())
			{
				Result->SetBoolField(TEXT("value"), Store.GetParameterValue<FNiagaraBool>(Variable).GetValue());
			}
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetVec2Def())
			{
				const FVector2f Value = Store.GetParameterValue<FVector2f>(Variable);
				TSharedRef<FJsonObject> ValueJson = MakeShared<FJsonObject>();
				ValueJson->SetNumberField(TEXT("x"), Value.X);
				ValueJson->SetNumberField(TEXT("y"), Value.Y);
				Result->SetObjectField(TEXT("value"), ValueJson);
			}
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetVec3Def())
			{
				const FVector3f Value = Store.GetParameterValue<FVector3f>(Variable);
				TSharedRef<FJsonObject> ValueJson = MakeShared<FJsonObject>();
				ValueJson->SetNumberField(TEXT("x"), Value.X);
				ValueJson->SetNumberField(TEXT("y"), Value.Y);
				ValueJson->SetNumberField(TEXT("z"), Value.Z);
				Result->SetObjectField(TEXT("value"), ValueJson);
			}
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetVec4Def())
			{
				const FVector4f Value = Store.GetParameterValue<FVector4f>(Variable);
				TSharedRef<FJsonObject> ValueJson = MakeShared<FJsonObject>();
				ValueJson->SetNumberField(TEXT("x"), Value.X);
				ValueJson->SetNumberField(TEXT("y"), Value.Y);
				ValueJson->SetNumberField(TEXT("z"), Value.Z);
				ValueJson->SetNumberField(TEXT("w"), Value.W);
				Result->SetObjectField(TEXT("value"), ValueJson);
			}
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetColorDef())
			{
				Result->SetObjectField(TEXT("value"), LinearColorToJson(Store.GetParameterValue<FLinearColor>(Variable)));
			}
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetQuatDef())
			{
				const FQuat4f Value = Store.GetParameterValue<FQuat4f>(Variable);
				TSharedRef<FJsonObject> ValueJson = MakeShared<FJsonObject>();
				ValueJson->SetNumberField(TEXT("x"), Value.X);
				ValueJson->SetNumberField(TEXT("y"), Value.Y);
				ValueJson->SetNumberField(TEXT("z"), Value.Z);
				ValueJson->SetNumberField(TEXT("w"), Value.W);
				Result->SetObjectField(TEXT("value"), ValueJson);
			}
			return Result;
		}

		TSharedRef<FJsonObject> NiagaraUserParametersToJson(UNiagaraSystem* System)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ParametersJson;
			if (System)
			{
				TArray<FNiagaraVariable> Parameters;
				System->GetExposedParameters().GetUserParameters(Parameters);
				for (const FNiagaraVariable& Parameter : Parameters)
				{
					ParametersJson.Add(MakeShared<FJsonValueObject>(NiagaraVariableToJson(System->GetExposedParameters(), Parameter)));
				}
			}
			Result->SetArrayField(TEXT("parameters"), ParametersJson);
			Result->SetNumberField(TEXT("count"), ParametersJson.Num());
			return Result;
		}

		bool FindNiagaraUserParameter(UNiagaraSystem* System, const FString& ParameterName, FNiagaraVariable& OutVariable)
		{
			if (!System)
			{
				return false;
			}

			TArray<FNiagaraVariable> Parameters;
			System->GetExposedParameters().GetUserParameters(Parameters);
			if (const FNiagaraVariable* Found = Parameters.FindByPredicate([&ParameterName](const FNiagaraVariable& Variable)
			{
				return Variable.GetName().ToString() == ParameterName || Variable.GetName().ToString() == FString::Printf(TEXT("User.%s"), *ParameterName);
			}))
			{
				OutVariable = *Found;
				return true;
			}
			return false;
		}

		bool SetNiagaraUserParameterValue(UNiagaraSystem* System, const FNiagaraVariable& Variable, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
		{
			if (!System)
			{
				OutError = TEXT("Niagara system is null.");
				return false;
			}

			FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			// SetParameterData expects raw byte pointer; reinterpret_cast to const uint8* is standard Niagara API usage.
			if (Variable.GetType() == FNiagaraTypeDefinition::GetFloatDef())
			{
				double Value = 0.0;
				if (!Arguments->TryGetNumberField(TEXT("value"), Value))
				{
					OutError = TEXT("value must be a number.");
					return false;
				}
				const float FloatValue = static_cast<float>(Value);
				Store.SetParameterData(reinterpret_cast<const uint8*>(&FloatValue), Variable);
				return true;
			}
			if (Variable.GetType() == FNiagaraTypeDefinition::GetIntDef())
			{
				int32 Value = 0;
				if (!Arguments->TryGetNumberField(TEXT("value"), Value))
				{
					OutError = TEXT("value must be an integer.");
					return false;
				}
				Store.SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable);
				return true;
			}
			if (Variable.GetType() == FNiagaraTypeDefinition::GetBoolDef())
			{
				if (!Arguments->HasTypedField<EJson::Boolean>(TEXT("value")))
				{
					OutError = TEXT("value must be a boolean.");
					return false;
				}
				const FNiagaraBool Value(Arguments->GetBoolField(TEXT("value")));
				Store.SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable);
				return true;
			}
			if (Variable.GetType() == FNiagaraTypeDefinition::GetVec2Def())
			{
				TSharedPtr<FJsonObject> ValueObject;
				if (!TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("value must be a vector2 object.");
					return false;
				}
				double X = 0.0;
				double Y = 0.0;
				if (!ValueObject->TryGetNumberField(TEXT("x"), X) || !ValueObject->TryGetNumberField(TEXT("y"), Y))
				{
					OutError = TEXT("value must contain ? and y.");
					return false;
				}
				const FVector2f Value(
					static_cast<float>(X),
					static_cast<float>(Y));
				Store.SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable);
				return true;
			}
			if (Variable.GetType() == FNiagaraTypeDefinition::GetVec3Def())
			{
				TSharedPtr<FJsonObject> ValueObject;
				if (!TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("value must be a vector object.");
					return false;
				}
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				if (!ValueObject->TryGetNumberField(TEXT("x"), X)
					|| !ValueObject->TryGetNumberField(TEXT("y"), Y)
					|| !ValueObject->TryGetNumberField(TEXT("z"), Z))
				{
					OutError = TEXT("value must be a vector object.");
					return false;
				}
				const FVector3f Value(
					static_cast<float>(X),
					static_cast<float>(Y),
					static_cast<float>(Z));
				Store.SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable);
				return true;
			}
			if (Variable.GetType() == FNiagaraTypeDefinition::GetVec4Def())
			{
				TSharedPtr<FJsonObject> ValueObject;
				if (!TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("value must be a vector4 object.");
					return false;
				}
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				double W = 0.0;
				if (!ValueObject->TryGetNumberField(TEXT("x"), X) || !ValueObject->TryGetNumberField(TEXT("y"), Y) || !ValueObject->TryGetNumberField(TEXT("z"), Z) || !ValueObject->TryGetNumberField(TEXT("w"), W))
				{
					OutError = TEXT("value must contain x, y, z and w.");
					return false;
				}
				const FVector4f Value(
					static_cast<float>(X),
					static_cast<float>(Y),
					static_cast<float>(Z),
					static_cast<float>(W));
				Store.SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable);
				return true;
			}
			if (Variable.GetType() == FNiagaraTypeDefinition::GetColorDef())
			{
				TSharedPtr<FJsonObject> ValueObject;
				if (!TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("value must be a color object.");
					return false;
				}
				FLinearColor Value;
				if (!FSololmcpEditorServices::JsonToLinearColor(ValueObject, Value))
				{
					OutError = TEXT("value must be a color object.");
					return false;
				}
				Store.SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable);
				return true;
			}
			if (Variable.GetType() == FNiagaraTypeDefinition::GetQuatDef())
			{
				TSharedPtr<FJsonObject> ValueObject;
				if (!TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("value must be a quat object.");
					return false;
				}
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				double W = 1.0;
				if (!ValueObject->TryGetNumberField(TEXT("x"), X) || !ValueObject->TryGetNumberField(TEXT("y"), Y) || !ValueObject->TryGetNumberField(TEXT("z"), Z) || !ValueObject->TryGetNumberField(TEXT("w"), W))
				{
					OutError = TEXT("value must contain x, y, z and w.");
					return false;
				}
				const FQuat4f Value(
					static_cast<float>(X),
					static_cast<float>(Y),
					static_cast<float>(Z),
					static_cast<float>(W));
				Store.SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable);
				return true;
			}

			OutError = TEXT("Unsupported Niagara user parameter type.");
			return false;
		}

		template<typename TEnum>
		bool TryParseNamedEnum(const TMap<FString, TEnum>& ValueMap, const FString& ValueName, TEnum& OutValue)
		{
			if (const TEnum* Found = ValueMap.Find(ValueName))
			{
				OutValue = *Found;
				return true;
			}
			return false;
		}

		bool TryParseNiagaraSpriteAlignment(const FString& ValueName, ENiagaraSpriteAlignment& OutValue)
		{
			static const TMap<FString, ENiagaraSpriteAlignment> ValueMap = {
				{TEXT("automatic"), ENiagaraSpriteAlignment::Automatic},
				{TEXT("custom_alignment"), ENiagaraSpriteAlignment::CustomAlignment},
				{TEXT("velocity_aligned"), ENiagaraSpriteAlignment::VelocityAligned},
				{TEXT("unaligned"), ENiagaraSpriteAlignment::Unaligned}
			};
			return TryParseNamedEnum(ValueMap, ValueName, OutValue);
		}

		bool TryParseNiagaraSpriteFacingMode(const FString& ValueName, ENiagaraSpriteFacingMode& OutValue)
		{
			static const TMap<FString, ENiagaraSpriteFacingMode> ValueMap = {
				{TEXT("automatic"), ENiagaraSpriteFacingMode::Automatic},
				{TEXT("custom_facing_vector"), ENiagaraSpriteFacingMode::CustomFacingVector},
				{TEXT("face_camera"), ENiagaraSpriteFacingMode::FaceCamera},
				{TEXT("face_camera_plane"), ENiagaraSpriteFacingMode::FaceCameraPlane},
				{TEXT("face_camera_position"), ENiagaraSpriteFacingMode::FaceCameraPosition},
				{TEXT("face_camera_distance_blend"), ENiagaraSpriteFacingMode::FaceCameraDistanceBlend}
			};
			return TryParseNamedEnum(ValueMap, ValueName, OutValue);
		}

		bool TryParseNiagaraSortMode(const FString& ValueName, ENiagaraSortMode& OutValue)
		{
			static const TMap<FString, ENiagaraSortMode> ValueMap = {
				{TEXT("none"), ENiagaraSortMode::None},
				{TEXT("view_depth"), ENiagaraSortMode::ViewDepth},
				{TEXT("view_distance"), ENiagaraSortMode::ViewDistance},
				{TEXT("custom_ascending"), ENiagaraSortMode::CustomAscending},
				{TEXT("custom_descending"), ENiagaraSortMode::CustomDecending}
			};
			return TryParseNamedEnum(ValueMap, ValueName, OutValue);
		}

		bool TryParseNiagaraMeshFacingMode(const FString& ValueName, ENiagaraMeshFacingMode& OutValue)
		{
			static const TMap<FString, ENiagaraMeshFacingMode> ValueMap = {
				{TEXT("default"), ENiagaraMeshFacingMode::Default},
				{TEXT("velocity"), ENiagaraMeshFacingMode::Velocity},
				{TEXT("camera_position"), ENiagaraMeshFacingMode::CameraPosition},
				{TEXT("camera_plane"), ENiagaraMeshFacingMode::CameraPlane}
			};
			return TryParseNamedEnum(ValueMap, ValueName, OutValue);
		}

		bool TryParseNiagaraRibbonFacingMode(const FString& ValueName, ENiagaraRibbonFacingMode& OutValue)
		{
			static const TMap<FString, ENiagaraRibbonFacingMode> ValueMap = {
				{TEXT("screen"), ENiagaraRibbonFacingMode::Screen},
				{TEXT("custom"), ENiagaraRibbonFacingMode::Custom},
				{TEXT("custom_side_vector"), ENiagaraRibbonFacingMode::CustomSideVector}
			};
			return TryParseNamedEnum(ValueMap, ValueName, OutValue);
		}

		bool TryParseNiagaraRibbonDrawDirection(const FString& ValueName, ENiagaraRibbonDrawDirection& OutValue)
		{
			static const TMap<FString, ENiagaraRibbonDrawDirection> ValueMap = {
				{TEXT("front_to_back"), ENiagaraRibbonDrawDirection::FrontToBack},
				{TEXT("back_to_front"), ENiagaraRibbonDrawDirection::BackToFront}
			};
			return TryParseNamedEnum(ValueMap, ValueName, OutValue);
		}

		bool TryParseNiagaraRibbonShapeMode(const FString& ValueName, ENiagaraRibbonShapeMode& OutValue)
		{
			static const TMap<FString, ENiagaraRibbonShapeMode> ValueMap = {
				{TEXT("plane"), ENiagaraRibbonShapeMode::Plane},
				{TEXT("multi_plane"), ENiagaraRibbonShapeMode::MultiPlane},
				{TEXT("tube"), ENiagaraRibbonShapeMode::Tube},
				{TEXT("custom"), ENiagaraRibbonShapeMode::Custom}
			};
			return TryParseNamedEnum(ValueMap, ValueName, OutValue);
		}

		bool TryParseNiagaraRibbonTessellationMode(const FString& ValueName, ENiagaraRibbonTessellationMode& OutValue)
		{
			static const TMap<FString, ENiagaraRibbonTessellationMode> ValueMap = {
				{TEXT("disabled"), ENiagaraRibbonTessellationMode::Disabled},
				{TEXT("automatic"), ENiagaraRibbonTessellationMode::Automatic},
				{TEXT("custom"), ENiagaraRibbonTessellationMode::Custom}
			};
			return TryParseNamedEnum(ValueMap, ValueName, OutValue);
		}

		ACineCameraActor* ResolveCineCameraActor(FSololmcpEditorServices& Services, const FString& ActorId, FString& OutError)
		{
			AActor* Actor = Services.FindActorByLabelOrName(ActorId, OutError);
			if (!Actor)
			{
				return nullptr;
			}

			ACineCameraActor* CineCameraActor = Cast<ACineCameraActor>(Actor);
			if (!CineCameraActor)
			{
				OutError = TEXT("Actor is not a cine camera actor.");
			}
			return CineCameraActor;
		}

		ULightComponent* ResolveLightComponent(FSololmcpEditorServices& Services, const FString& ActorId, FString& OutError)
		{
			AActor* Actor = Services.FindActorByLabelOrName(ActorId, OutError);
			if (!Actor)
			{
				return nullptr;
			}
			ULightComponent* Component = Actor->FindComponentByClass<ULightComponent>();
			if (!Component)
			{
				OutError = TEXT("Actor does not expose a light component.");
			}
			return Component;
		}

		UCameraComponent* ResolveCameraComponent(FSololmcpEditorServices& Services, const FString& ActorId, FString& OutError)
		{
			AActor* Actor = Services.FindActorByLabelOrName(ActorId, OutError);
			if (!Actor)
			{
				return nullptr;
			}
			if (ACameraActor* CameraActor = Cast<ACameraActor>(Actor))
			{
				return CameraActor->GetCameraComponent();
			}
			if (ACineCameraActor* CineCameraActor = Cast<ACineCameraActor>(Actor))
			{
				return CineCameraActor->GetCameraComponent();
			}
			UCameraComponent* Component = Actor->FindComponentByClass<UCameraComponent>();
			if (!Component)
			{
				OutError = TEXT("Actor does not expose a camera component.");
			}
			return Component;
		}

		FString DataLayerTypeToString(const EDataLayerType DataLayerType)
		{
			switch (DataLayerType)
			{
			case EDataLayerType::Runtime:
				return TEXT("runtime");
			case EDataLayerType::Editor:
				return TEXT("editor");
			default:
				return TEXT("unknown");
			}
		}

		FString DataLayerRuntimeStateToString(const EDataLayerRuntimeState RuntimeState)
		{
			switch (RuntimeState)
			{
			case EDataLayerRuntimeState::Unloaded:
				return TEXT("unloaded");
			case EDataLayerRuntimeState::Loaded:
				return TEXT("loaded");
			case EDataLayerRuntimeState::Activated:
				return TEXT("activated");
			default:
				return TEXT("unknown");
			}
		}

		bool DataLayerMatchesIdentifier(const UDataLayerInstance* DataLayer, const FString& Identifier)
		{
			if (!DataLayer || Identifier.IsEmpty())
			{
				return false;
			}

			if (DataLayer->GetDataLayerShortName().Equals(Identifier, ESearchCase::IgnoreCase) ||
				DataLayer->GetDataLayerFullName().Equals(Identifier, ESearchCase::IgnoreCase) ||
				DataLayer->GetPathName().Equals(Identifier, ESearchCase::IgnoreCase))
			{
				return true;
			}

			if (const UDataLayerAsset* DataLayerAsset = DataLayer->GetAsset())
			{
				if (DataLayerAsset->GetPathName().Equals(Identifier, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}

			return false;
		}

		TSharedRef<FJsonObject> DataLayerInstanceToJson(const UDataLayerInstance* DataLayer)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!DataLayer)
			{
				return Result;
			}

			Result->SetObjectField(TEXT("object"), FSololmcpEditorServices::MakeObjectReference(DataLayer));
			Result->SetStringField(TEXT("name"), DataLayer->GetDataLayerShortName());
			Result->SetStringField(TEXT("shortName"), DataLayer->GetDataLayerShortName());
			Result->SetStringField(TEXT("fullName"), DataLayer->GetDataLayerFullName());
			Result->SetStringField(TEXT("type"), DataLayerTypeToString(DataLayer->GetType()));
			Result->SetBoolField(TEXT("runtime"), DataLayer->IsRuntime());
			Result->SetBoolField(TEXT("visible"), DataLayer->IsVisible());
			Result->SetBoolField(TEXT("loadedInEditor"), DataLayer->IsLoadedInEditor());
			Result->SetBoolField(TEXT("initiallyVisible"), DataLayer->IsInitiallyVisible());
			Result->SetStringField(TEXT("initialRuntimeState"), DataLayerRuntimeStateToString(DataLayer->GetInitialRuntimeState()));

			if (const UDataLayerAsset* DataLayerAsset = DataLayer->GetAsset())
			{
				Result->SetStringField(TEXT("assetPath"), DataLayerAsset->GetPathName());
				Result->SetObjectField(TEXT("asset"), FSololmcpEditorServices::MakeObjectReference(DataLayerAsset));
				Result->SetBoolField(TEXT("assetIsPrivate"), DataLayerAsset->IsPrivate());
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				// UDataLayerAsset gained these in 5.4. Omitting the fields on 5.3 is better
				// than defaulting them: a caller can tell "not reported" from "false".
				Result->SetBoolField(TEXT("assetIsClientOnly"), DataLayerAsset->IsClientOnly());
				Result->SetBoolField(TEXT("assetIsServerOnly"), DataLayerAsset->IsServerOnly());
#endif
			}

			if (const UDataLayerInstance* Parent = DataLayer->GetParent())
			{
				Result->SetObjectField(TEXT("parent"), FSololmcpEditorServices::MakeObjectReference(Parent));
				Result->SetStringField(TEXT("parentFullName"), Parent->GetDataLayerFullName());
			}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
			// UExternalDataLayerInstance and GetRootExternalDataLayerInstance arrived in
			// 5.4. The field is omitted on 5.3 rather than reported as null, so a caller
			// can distinguish "no external data layer" from "this engine cannot say".
			if (const UExternalDataLayerInstance* RootExternalDataLayerInstance = DataLayer->GetRootExternalDataLayerInstance())
			{
				Result->SetObjectField(TEXT("rootExternalDataLayer"), FSololmcpEditorServices::MakeObjectReference(RootExternalDataLayerInstance));
			}
#endif

			return Result;
		}

		UDataLayerInstance* ResolveDataLayerInstance(const FString& DataLayerName, FString& OutError)
		{
			if (!GEditor)
			{
				OutError = TEXT("Editor is unavailable.");
				return nullptr;
			}

			UDataLayerEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>();
			if (!Subsystem)
			{
				OutError = TEXT("DataLayerEditorSubsystem is unavailable.");
				return nullptr;
			}

			UDataLayerManager* DataLayerManager = UDataLayerManager::GetDataLayerManager(GEditor->GetEditorWorldContext().World());
			if (!DataLayerManager)
			{
				OutError = TEXT("Current world does not expose a data layer manager.");
				return nullptr;
			}

			UDataLayerInstance* DataLayer = nullptr;
			int32 MatchCount = 0;
			DataLayerManager->ForEachDataLayerInstance([&](UDataLayerInstance* Candidate)
			{
				if (DataLayerMatchesIdentifier(Candidate, DataLayerName))
				{
					DataLayer = Candidate;
					++MatchCount;
				}
				return true;
			});

			if (!DataLayer)
			{
				OutError = TEXT("Data layer was not found.");
				return nullptr;
			}
			if (MatchCount > 1)
			{
				OutError = TEXT("Data layer identifier is ambiguous. Please use full_name, asset_path, or object_path.");
				return nullptr;
			}
			return DataLayer;
		}

		TSharedRef<FJsonObject> MakeDataLayerListResult(const TArray<UDataLayerInstance*>& DataLayers)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> DataLayerArray;
			for (const UDataLayerInstance* DataLayer : DataLayers)
			{
				if (DataLayer)
				{
					DataLayerArray.Add(MakeShared<FJsonValueObject>(DataLayerInstanceToJson(DataLayer)));
				}
			}
			Result->SetArrayField(TEXT("dataLayers"), DataLayerArray);
			Result->SetNumberField(TEXT("count"), DataLayerArray.Num());
			return Result;
		}

		UDataLayerInstance* ResolveDataLayerInstanceFromArguments(const TSharedRef<FJsonObject>& Arguments, const TCHAR* PreferredFieldName, FString& OutError)
		{
			static const TCHAR* CandidateFields[] =
			{
				TEXT("data_layer_object_path"),
				TEXT("object_path"),
				TEXT("data_layer_full_name"),
				TEXT("full_name"),
				TEXT("data_layer_asset_path"),
				TEXT("asset_path"),
				TEXT("data_layer_name"),
				TEXT("name")
			};

			FString Identifier;
			if (PreferredFieldName && Arguments->TryGetStringField(PreferredFieldName, Identifier) && !Identifier.IsEmpty())
			{
				return ResolveDataLayerInstance(Identifier, OutError);
			}

			for (const TCHAR* CandidateField : CandidateFields)
			{
				if (PreferredFieldName && FCString::Stricmp(PreferredFieldName, CandidateField) == 0)
				{
					continue;
				}
				if (Arguments->TryGetStringField(CandidateField, Identifier) && !Identifier.IsEmpty())
				{
					return ResolveDataLayerInstance(Identifier, OutError);
				}
			}

			OutError = TEXT("Missing data layer identifier.");
			return nullptr;
		}

		TSharedPtr<FSololmcpManagedStreamingSourceProvider> FindManagedStreamingSourceProvider(UWorld* World, const FString& SourceIdOrName)
		{
			for (const TSharedPtr<FSololmcpManagedStreamingSourceProvider>& Provider : GetManagedStreamingSourceProviders())
			{
				if (!Provider.IsValid() || Provider->World.Get() != World)
				{
					continue;
				}
				if (Provider->SourceId.Equals(SourceIdOrName, ESearchCase::IgnoreCase) || Provider->Source.Name.ToString().Equals(SourceIdOrName, ESearchCase::IgnoreCase))
				{
					return Provider;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> VectorToJson(const FVector& Vector);
		TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform);
		TSharedRef<FJsonObject> BoxToJson(const FBox& Box);

		UWorldPartition* GetCurrentEditorWorldPartition(FString& OutError)
		{
			if (!GEditor)
			{
				OutError = TEXT("Editor is unavailable.");
				return nullptr;
			}

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				OutError = TEXT("Editor world is unavailable.");
				return nullptr;
			}

			UWorldPartition* WorldPartition = World->GetWorldPartition();
			if (!WorldPartition)
			{
				OutError = TEXT("World partition is unavailable.");
			}
			return WorldPartition;
		}

		TSharedRef<FJsonObject> RotatorToJson(const FRotator& Rotator)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("pitch"), Rotator.Pitch);
			Result->SetNumberField(TEXT("yaw"), Rotator.Yaw);
			Result->SetNumberField(TEXT("roll"), Rotator.Roll);
			return Result;
		}

		FString StreamingSourceTargetBehaviorToString(const EStreamingSourceTargetBehavior Behavior)
		{
			switch (Behavior)
			{
			case EStreamingSourceTargetBehavior::Include:
				return TEXT("include");
			case EStreamingSourceTargetBehavior::Exclude:
				return TEXT("exclude");
			default:
				return TEXT("unknown");
			}
		}

		FString StreamingSourcePriorityToString(const EStreamingSourcePriority Priority)
		{
			switch (Priority)
			{
			case EStreamingSourcePriority::Highest:
				return TEXT("highest");
			case EStreamingSourcePriority::High:
				return TEXT("high");
			case EStreamingSourcePriority::Normal:
				return TEXT("normal");
			case EStreamingSourcePriority::Low:
				return TEXT("low");
			case EStreamingSourcePriority::Lowest:
				return TEXT("lowest");
			default:
				return TEXT("normal");
			}
		}

		TSharedRef<FJsonObject> StreamingSourceToJson(const FWorldPartitionStreamingSource& Source)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), Source.Name.ToString());
			Result->SetObjectField(TEXT("location"), VectorToJson(Source.Location));
			Result->SetObjectField(TEXT("rotation"), RotatorToJson(Source.Rotation));
			Result->SetStringField(TEXT("targetState"), FString(GetStreamingSourceTargetStateName(Source.TargetState)).ToLower());
			Result->SetBoolField(TEXT("blockOnSlowLoading"), Source.bBlockOnSlowLoading);
			Result->SetStringField(TEXT("priority"), StreamingSourcePriorityToString(Source.Priority));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
			Result->SetObjectField(TEXT("velocity"), VectorToJson(Source.Velocity));
#else
			// 5.3 stores Velocity as a scalar speed; 5.4 widened it to an FVector.
			// Reported as a number here rather than a fabricated vector.
			Result->SetNumberField(TEXT("velocity"), Source.Velocity);
#endif
			Result->SetStringField(TEXT("targetBehavior"), StreamingSourceTargetBehaviorToString(Source.TargetBehavior));
			Result->SetBoolField(TEXT("remote"), Source.bRemote);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			Result->SetBoolField(TEXT("force2D"), Source.bForce2D);
#else
			// FWorldPartitionStreamingSource::bForce2D is 5.5+.
#endif

			TArray<TSharedPtr<FJsonValue>> TargetGrids;
			for (const FName& GridName : Source.TargetGrids)
			{
				TargetGrids.Add(MakeShared<FJsonValueString>(GridName.ToString()));
			}
			Result->SetArrayField(TEXT("targetGrids"), TargetGrids);
			Result->SetNumberField(TEXT("shapeCount"), Source.Shapes.Num());
			return Result;
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
		// Reports fields that only exist from 5.4: GetDataLayerInstanceNames returns a
		// wrapper with ToArray() there and a plain array on 5.3, and HasContentBundle,
		// GetHLODLayer and GetEditorBounds are absent. Callers on 5.3 reach the
		// pre-instance actor-desc path, which this file already gates separately.
		TSharedRef<FJsonObject> ActorDescInstanceToJson(const SOMOLMCP_ACTOR_DESC* ActorDescInstance)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!ActorDescInstance)
			{
				return Result;
			}

			Result->SetStringField(TEXT("guid"), ActorDescInstance->GetGuid().ToString());
			Result->SetStringField(TEXT("label"), ActorDescInstance->GetActorLabel().ToString());
			Result->SetStringField(TEXT("name"), ActorDescInstance->GetActorName().ToString());
			Result->SetStringField(TEXT("labelOrName"), ActorDescInstance->GetActorLabelOrName().ToString());
			Result->SetStringField(TEXT("nativeClass"), ActorDescInstance->GetNativeClass().ToString());
			Result->SetStringField(TEXT("displayClass"), ActorDescInstance->GetDisplayClassName().ToString());
			Result->SetStringField(TEXT("runtimeGrid"), ActorDescInstance->GetRuntimeGrid().ToString());
			Result->SetStringField(TEXT("actorPackage"), ActorDescInstance->GetActorPackage().ToString());
			Result->SetStringField(TEXT("actorPath"), ActorDescInstance->GetActorSoftPath().ToString());
			Result->SetStringField(TEXT("folderPath"), ActorDescInstance->GetFolderPath().ToString());
			Result->SetBoolField(TEXT("spatiallyLoaded"), ActorDescInstance->GetIsSpatiallyLoaded());
			Result->SetBoolField(TEXT("editorOnly"), ActorDescInstance->GetActorIsEditorOnly());
			Result->SetBoolField(TEXT("runtimeOnly"), ActorDescInstance->GetActorIsRuntimeOnly());
			Result->SetBoolField(TEXT("loaded"), ActorDescInstance->IsLoaded());
			Result->SetBoolField(TEXT("hlodRelevant"), ActorDescInstance->GetActorIsHLODRelevant());
			Result->SetStringField(TEXT("hlodLayer"), ActorDescInstance->GetHLODLayer().ToString());
			Result->SetStringField(TEXT("contentBundleGuid"), ActorDescInstance->GetContentBundleGuid().ToString());
			Result->SetObjectField(TEXT("transform"), TransformToJson(ActorDescInstance->GetActorTransform()));
			Result->SetObjectField(TEXT("editorBounds"), BoxToJson(ActorDescInstance->GetEditorBounds()));

			TArray<TSharedPtr<FJsonValue>> DataLayerNames;
			for (const FName& DataLayerName : ActorDescInstance->GetDataLayerInstanceNames().ToArray())
			{
				DataLayerNames.Add(MakeShared<FJsonValueString>(DataLayerName.ToString()));
			}
			Result->SetArrayField(TEXT("dataLayerInstanceNames"), DataLayerNames);

			if (AActor* LoadedActor = ActorDescInstance->GetActor())
			{
				Result->SetObjectField(TEXT("loadedActor"), FSololmcpEditorServices::MakeActorReference(LoadedActor));
			}

			return Result;
		}
#else
		// 5.3 has the same descriptor accessors under the pre-instance type, with two
		// differences: GetDataLayerInstanceNames returns a plain TArray rather than a
		// wrapper with ToArray(), and there is no public actor-transform accessor -- the
		// transform is a private member there, so that one field is omitted rather than
		// guessed. Everything else is reported identically.
		TSharedRef<FJsonObject> ActorDescInstanceToJson(const SOMOLMCP_ACTOR_DESC* ActorDescInstance)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!ActorDescInstance)
			{
				return Result;
			}

			Result->SetStringField(TEXT("guid"), ActorDescInstance->GetGuid().ToString());
			Result->SetStringField(TEXT("label"), ActorDescInstance->GetActorLabel().ToString());
			Result->SetStringField(TEXT("name"), ActorDescInstance->GetActorName().ToString());
			Result->SetStringField(TEXT("labelOrName"), ActorDescInstance->GetActorLabelOrName().ToString());
			Result->SetStringField(TEXT("nativeClass"), ActorDescInstance->GetNativeClass().ToString());
			Result->SetStringField(TEXT("displayClass"), ActorDescInstance->GetDisplayClassName().ToString());
			Result->SetStringField(TEXT("runtimeGrid"), ActorDescInstance->GetRuntimeGrid().ToString());
			Result->SetStringField(TEXT("actorPackage"), ActorDescInstance->GetActorPackage().ToString());
			Result->SetStringField(TEXT("actorPath"), ActorDescInstance->GetActorSoftPath().ToString());
			Result->SetStringField(TEXT("folderPath"), ActorDescInstance->GetFolderPath().ToString());
			Result->SetBoolField(TEXT("spatiallyLoaded"), ActorDescInstance->GetIsSpatiallyLoaded());
			Result->SetBoolField(TEXT("editorOnly"), ActorDescInstance->GetActorIsEditorOnly());
			Result->SetBoolField(TEXT("runtimeOnly"), ActorDescInstance->GetActorIsRuntimeOnly());
			Result->SetBoolField(TEXT("loaded"), ActorDescInstance->IsLoaded());
			Result->SetBoolField(TEXT("hlodRelevant"), ActorDescInstance->GetActorIsHLODRelevant());
			Result->SetStringField(TEXT("hlodLayer"), ActorDescInstance->GetHLODLayer().ToString());
			Result->SetStringField(TEXT("contentBundleGuid"), ActorDescInstance->GetContentBundleGuid().ToString());
			Result->SetObjectField(TEXT("editorBounds"), BoxToJson(ActorDescInstance->GetEditorBounds()));

			TArray<TSharedPtr<FJsonValue>> DataLayerNames;
			for (const FName& DataLayerName : ActorDescInstance->GetDataLayerInstanceNames())
			{
				DataLayerNames.Add(MakeShared<FJsonValueString>(DataLayerName.ToString()));
			}
			Result->SetArrayField(TEXT("dataLayerInstanceNames"), DataLayerNames);

			if (AActor* LoadedActor = ActorDescInstance->GetActor())
			{
				Result->SetObjectField(TEXT("loadedActor"), FSololmcpEditorServices::MakeActorReference(LoadedActor));
			}

			return Result;
		}
#endif

		FString RuntimeCellStateToString(const EWorldPartitionRuntimeCellState State)
		{
			switch (State)
			{
			case EWorldPartitionRuntimeCellState::Unloaded:
				return TEXT("unloaded");
			case EWorldPartitionRuntimeCellState::Loaded:
				return TEXT("loaded");
			case EWorldPartitionRuntimeCellState::Activated:
				return TEXT("activated");
			default:
				return TEXT("unknown");
			}
		}

		TSharedRef<FJsonObject> RuntimeCellToJson(const IWorldPartitionCell* Cell)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Cell)
			{
				return Result;
			}

			Result->SetStringField(TEXT("debugName"), Cell->GetDebugName());
			Result->SetStringField(TEXT("levelPackageName"), Cell->GetLevelPackageName().ToString());
			Result->SetObjectField(TEXT("cellBounds"), BoxToJson(Cell->GetCellBounds()));
			Result->SetObjectField(TEXT("contentBounds"), BoxToJson(Cell->GetContentBounds()));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
			Result->SetBoolField(TEXT("hasContentBundle"), Cell->HasContentBundle());
			Result->SetStringField(TEXT("externalDataLayer"), Cell->GetExternalDataLayer().ToString());
#else
			// IWorldPartitionCell gained HasContentBundle and GetExternalDataLayer in 5.4.
#endif

			TArray<TSharedPtr<FJsonValue>> DataLayersJson;
			for (const FName& DataLayerName : Cell->GetDataLayers())
			{
				DataLayersJson.Add(MakeShared<FJsonValueString>(DataLayerName.ToString()));
			}
			Result->SetArrayField(TEXT("dataLayers"), DataLayersJson);

			if (const UWorldPartitionRuntimeCell* RuntimeCell = Cast<UWorldPartitionRuntimeCell>(Cell))
			{
				Result->SetStringField(TEXT("guid"), RuntimeCell->GetGuid().ToString());
				Result->SetBoolField(TEXT("alwaysLoaded"), RuntimeCell->IsAlwaysLoaded());
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				Result->SetBoolField(TEXT("spatiallyLoaded"), RuntimeCell->IsSpatiallyLoaded());
#endif
				Result->SetBoolField(TEXT("blockOnSlowLoading"), RuntimeCell->GetBlockOnSlowLoading());
				Result->SetNumberField(TEXT("actorCount"), RuntimeCell->GetActorCount());
				Result->SetStringField(TEXT("currentState"), RuntimeCellStateToString(RuntimeCell->GetCurrentState()));
				Result->SetBoolField(TEXT("isHLOD"), RuntimeCell->GetIsHLOD());
				Result->SetStringField(TEXT("sourceCellGuid"), RuntimeCell->GetSourceCellGuid().ToString());
			}

			return Result;
		}

		bool TryGetRegionBoxFromArguments(const TSharedRef<FJsonObject>& Arguments, FBox& OutBox, FString& OutError)
		{
			TSharedPtr<FJsonObject> MinObject;
			TSharedPtr<FJsonObject> MaxObject;
			if (!TryGetObjectField(Arguments, TEXT("min"), MinObject) || !TryGetObjectField(Arguments, TEXT("max"), MaxObject))
			{
				OutError = TEXT("Missing min or max.");
				return false;
			}

			FVector Min;
			FVector Max;
			if (!FSololmcpEditorServices::JsonToVector(MinObject, Min) || !FSololmcpEditorServices::JsonToVector(MaxObject, Max))
			{
				OutError = TEXT("min/max must be vector objects.");
				return false;
			}

			OutBox = FBox(Min, Max);
			return true;
		}

		bool BoxesMatch(const FBox& A, const FBox& B)
		{
			return A.Min.Equals(B.Min, KINDA_SMALL_NUMBER) && A.Max.Equals(B.Max, KINDA_SMALL_NUMBER);
		}

		/** TerrainSpec / tile grid: XY plane in Unreal cm, Z carried through for documentation only. */
		bool TryGetTerrainWorldBoundsCm(const TSharedRef<FJsonObject>& Arguments, FVector& OutMin, FVector& OutMax, FString& OutError)
		{
			TSharedPtr<FJsonObject> MinObject;
			TSharedPtr<FJsonObject> MaxObject;
			if (!TryGetObjectField(Arguments, TEXT("world_min_cm"), MinObject) || !TryGetObjectField(Arguments, TEXT("world_max_cm"), MaxObject))
			{
				OutError = TEXT("Missing world_min_cm or world_max_cm.");
				return false;
			}
			if (!FSololmcpEditorServices::JsonToVector(MinObject, OutMin) || !FSololmcpEditorServices::JsonToVector(MaxObject, OutMax))
			{
				OutError = TEXT("world_min_cm / world_max_cm must be vector objects {x,y,z}.");
				return false;
			}
			return true;
		}

		/**
		 * Map a world-space XY AABB (cm) to inclusive landscape heightmap vertex indices.
		 * Uses LandscapeActorToWorld + ActorScale3D (XY) — matches common single-actor landscapes;
		 * streaming proxies with non-uniform sectioning may need manual verification.
		 */
		bool TryWorldBoxToLandscapeHeightmapIndices(ALandscape* Landscape, const FVector& WorldMin, const FVector& WorldMax, int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY, FString& OutError)
		{
			if (!Landscape)
			{
				OutError = TEXT("Landscape is null.");
				return false;
			}

			const FTransform LandscapeToWorld = Landscape->LandscapeActorToWorld();
			const FVector Scale3D = Landscape->GetActorScale3D();
			const float Sx = FMath::Max(Scale3D.X, KINDA_SMALL_NUMBER);
			const float Sy = FMath::Max(Scale3D.Y, KINDA_SMALL_NUMBER);

			const FVector Corners[4] = {
				FVector(WorldMin.X, WorldMin.Y, 0.f),
				FVector(WorldMax.X, WorldMin.Y, 0.f),
				FVector(WorldMin.X, WorldMax.Y, 0.f),
				FVector(WorldMax.X, WorldMax.Y, 0.f)
			};

			float MinLocalX = FLT_MAX;
			float MinLocalY = FLT_MAX;
			float MaxLocalX = -FLT_MAX;
			float MaxLocalY = -FLT_MAX;
			for (const FVector& Corner : Corners)
			{
				const FVector Local = LandscapeToWorld.InverseTransformPosition(Corner);
				MinLocalX = FMath::Min(MinLocalX, Local.X);
				MaxLocalX = FMath::Max(MaxLocalX, Local.X);
				MinLocalY = FMath::Min(MinLocalY, Local.Y);
				MaxLocalY = FMath::Max(MaxLocalY, Local.Y);
			}

			const int32 RawMinX = FMath::FloorToInt(MinLocalX / Sx);
			const int32 RawMinY = FMath::FloorToInt(MinLocalY / Sy);
			const int32 RawMaxX = FMath::FloorToInt((MaxLocalX + Sx * 0.0001f) / Sx);
			const int32 RawMaxY = FMath::FloorToInt((MaxLocalY + Sy * 0.0001f) / Sy);

			const FIntRect Bounds = Landscape->GetBoundingRect();
			OutMinX = FMath::Clamp(FMath::Min(RawMinX, RawMaxX), Bounds.Min.X, Bounds.Max.X);
			OutMinY = FMath::Clamp(FMath::Min(RawMinY, RawMaxY), Bounds.Min.Y, Bounds.Max.Y);
			OutMaxX = FMath::Clamp(FMath::Max(RawMinX, RawMaxX), Bounds.Min.X, Bounds.Max.X);
			OutMaxY = FMath::Clamp(FMath::Max(RawMinY, RawMaxY), Bounds.Min.Y, Bounds.Max.Y);

			if (OutMinX > OutMaxX || OutMinY > OutMaxY)
			{
				OutError = TEXT("World box does not overlap this landscape heightmap (after clamp).");
				return false;
			}
			return true;
		}

		void BuildPerlinNoiseHeightBuffer(const int32 MinX, const int32 MinY, const int32 MaxX, const int32 MaxY, const int32 Seed, const float Frequency, const float Amplitude, TArray<uint16>& OutHeightData)
		{
			// FIX (v12): backward-compat dual-mode amplitude.
			//   - If Amplitude <= 4.0: legacy normalized [0..4] -> Span 0..65536
			//     (amplitude=4 saturates the full landscape height range)
			//   - If Amplitude > 4.0: raw-unit mode. Span = Amplitude directly (0..32767).
			//     amplitude=8000 -> ±8000 raw units around midpoint = ~25% of full range.
			//   Lets callers control mountain height by raw heightmap units (intuitive)
			//   without the surprising silent-clamp at 4.0.
			float Span;
			if (Amplitude <= 4.0f)
			{
				Span = 16384.f * FMath::Clamp(Amplitude, 0.f, 4.f);
			}
			else
			{
				Span = FMath::Min(Amplitude, 32767.f);
			}

			OutHeightData.Reset();
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const float SampleX = static_cast<float>(X) * Frequency + static_cast<float>(Seed) * 0.017f;
					const float SampleY = static_cast<float>(Y) * Frequency + static_cast<float>(Seed) * 0.031f;
					const float SampleZ = static_cast<float>(Seed) * 0.009f;
					const float Noise = FMath::PerlinNoise3D(FVector(SampleX, SampleY, SampleZ));
					const float HeightValue = 32768.f + Noise * Span;
					OutHeightData.Add(static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(HeightValue), 0, 65535)));
				}
			}
		}

		bool BuildTerrainTilePlanJson(const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutError)
		{
			FVector WorldMin;
			FVector WorldMax;
			if (!TryGetTerrainWorldBoundsCm(Arguments, WorldMin, WorldMax, OutError))
			{
				return false;
			}

			double CellSizeCm = 25600.0;
			if (Arguments->HasTypedField<EJson::Number>(TEXT("world_partition_cell_size_cm")))
			{
				CellSizeCm = Arguments->GetNumberField(TEXT("world_partition_cell_size_cm"));
			}
			if (CellSizeCm <= KINDA_SMALL_NUMBER)
			{
				OutError = TEXT("world_partition_cell_size_cm must be positive.");
				return false;
			}

			double OverlapCm = 0.0;
			if (Arguments->HasTypedField<EJson::Number>(TEXT("tile_overlap_cm")))
			{
				OverlapCm = Arguments->GetNumberField(TEXT("tile_overlap_cm"));
			}
			if (OverlapCm < 0.0 || OverlapCm >= CellSizeCm * 0.5 - KINDA_SMALL_NUMBER)
			{
				OutError = TEXT("tile_overlap_cm must be >= 0 and < half of world_partition_cell_size_cm.");
				return false;
			}

			const FVector Lo(
				FMath::Min(WorldMin.X, WorldMax.X),
				FMath::Min(WorldMin.Y, WorldMax.Y),
				FMath::Min(WorldMin.Z, WorldMax.Z));
			const FVector Hi(
				FMath::Max(WorldMin.X, WorldMax.X),
				FMath::Max(WorldMin.Y, WorldMax.Y),
				FMath::Max(WorldMin.Z, WorldMax.Z));

			const double S = CellSizeCm;
			const int32 IxLo = FMath::FloorToInt(Lo.X / S);
			const int32 IxHi = FMath::CeilToInt(Hi.X / S) - 1;
			const int32 IyLo = FMath::FloorToInt(Lo.Y / S);
			const int32 IyHi = FMath::CeilToInt(Hi.Y / S) - 1;

			if (IxLo > IxHi || IyLo > IyHi)
			{
				OutError = TEXT("World XY bounds do not intersect any grid cells (check coordinates vs cell size).");
				return false;
			}

			const auto MakeCmVectorJson = [](const FVector& V) -> TSharedRef<FJsonObject>
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetNumberField(TEXT("x"), V.X);
				J->SetNumberField(TEXT("y"), V.Y);
				J->SetNumberField(TEXT("z"), V.Z);
				return J;
			};

			TSharedRef<FJsonObject> GridJson = MakeShared<FJsonObject>();
			GridJson->SetNumberField(TEXT("world_partition_cell_size_cm"), CellSizeCm);
			GridJson->SetNumberField(TEXT("tile_overlap_cm"), OverlapCm);
			GridJson->SetNumberField(TEXT("index_x_min"), IxLo);
			GridJson->SetNumberField(TEXT("index_x_max"), IxHi);
			GridJson->SetNumberField(TEXT("index_y_min"), IyLo);
			GridJson->SetNumberField(TEXT("index_y_max"), IyHi);
			GridJson->SetNumberField(TEXT("tile_count_x"), IxHi - IxLo + 1);
			GridJson->SetNumberField(TEXT("tile_count_y"), IyHi - IyLo + 1);

			TArray<TSharedPtr<FJsonValue>> TilesJson;
			for (int32 Iy = IyLo; Iy <= IyHi; ++Iy)
			{
				for (int32 Ix = IxLo; Ix <= IxHi; ++Ix)
				{
					const double MinX = static_cast<double>(Ix) * S - OverlapCm;
					const double MaxX = static_cast<double>(Ix + 1) * S + OverlapCm;
					const double MinY = static_cast<double>(Iy) * S - OverlapCm;
					const double MaxY = static_cast<double>(Iy + 1) * S + OverlapCm;

					TSharedRef<FJsonObject> Tile = MakeShared<FJsonObject>();
					Tile->SetStringField(TEXT("id"), FString::Printf(TEXT("%d_%d"), Ix, Iy));
					Tile->SetNumberField(TEXT("grid_ix"), Ix);
					Tile->SetNumberField(TEXT("grid_iy"), Iy);
					Tile->SetObjectField(TEXT("bounds_min_cm"), MakeCmVectorJson(FVector(static_cast<float>(MinX), static_cast<float>(MinY), Lo.Z)));
					Tile->SetObjectField(TEXT("bounds_max_cm"), MakeCmVectorJson(FVector(static_cast<float>(MaxX), static_cast<float>(MaxY), Hi.Z)));
					Tile->SetObjectField(TEXT("core_cell_min_cm"), MakeCmVectorJson(FVector(static_cast<float>(Ix * S), static_cast<float>(Iy * S), Lo.Z)));
					Tile->SetObjectField(TEXT("core_cell_max_cm"), MakeCmVectorJson(FVector(static_cast<float>((Ix + 1) * S), static_cast<float>((Iy + 1) * S), Hi.Z)));
					TilesJson.Add(MakeShared<FJsonValueObject>(Tile));
				}
			}

			OutStructured = MakeShared<FJsonObject>();
			OutStructured->SetObjectField(TEXT("grid"), GridJson);
			OutStructured->SetArrayField(TEXT("tiles"), TilesJson);
			OutStructured->SetNumberField(TEXT("tile_count"), TilesJson.Num());
			OutStructured->SetStringField(TEXT("notes"), TEXT("Tiles are aligned to a square XY grid in cm; core_cell_* is the WP-style cell without overlap; bounds_* includes tile_overlap_cm for blended PCG/Landscape seams."));
			return true;
		}

		bool ValidateTerrainSpecJson(const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutError)
		{
			const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
			if (!Arguments->TryGetObjectField(TEXT("terrain_spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
			{
				OutError = TEXT("Missing terrain_spec object.");
				return false;
			}
			const TSharedRef<FJsonObject> Spec = SpecPtr->ToSharedRef();

			TArray<TSharedPtr<FJsonValue>> Issues;
			bool bOk = true;

			FVector WMin, WMax;
			FString BoundsErr;
			if (!TryGetTerrainWorldBoundsCm(Spec, WMin, WMax, BoundsErr))
			{
				bOk = false;
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("code"), TEXT("bounds"));
				Issue->SetStringField(TEXT("message"), BoundsErr);
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			}
			else
			{
				const FVector Lo(
					FMath::Min(WMin.X, WMax.X),
					FMath::Min(WMin.Y, WMax.Y),
					FMath::Min(WMin.Z, WMax.Z));
				const FVector Hi(
					FMath::Max(WMin.X, WMax.X),
					FMath::Max(WMin.Y, WMax.Y),
					FMath::Max(WMin.Z, WMax.Z));
				if (Lo.X >= Hi.X || Lo.Y >= Hi.Y)
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("bounds_xy_order"));
					Issue->SetStringField(TEXT("message"), TEXT("Terrain world bounds must have positive extent on X and Y (min corner vs max corner)."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}

			double CellSize = 0.0;
			if (!Spec->HasTypedField<EJson::Number>(TEXT("world_partition_cell_size_cm")))
			{
				bOk = false;
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("code"), TEXT("missing_cell_size"));
				Issue->SetStringField(TEXT("message"), TEXT("terrain_spec.world_partition_cell_size_cm is required."));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			}
			else
			{
				CellSize = Spec->GetNumberField(TEXT("world_partition_cell_size_cm"));
				if (CellSize <= KINDA_SMALL_NUMBER)
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("cell_size"));
					Issue->SetStringField(TEXT("message"), TEXT("world_partition_cell_size_cm must be positive."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}

			if (Spec->HasTypedField<EJson::Number>(TEXT("tile_overlap_cm")))
			{
				const double Overlap = Spec->GetNumberField(TEXT("tile_overlap_cm"));
				if (Overlap < 0.0 || (CellSize > 0.0 && Overlap >= CellSize * 0.5))
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("overlap"));
					Issue->SetStringField(TEXT("message"), TEXT("tile_overlap_cm must be >= 0 and < half of world_partition_cell_size_cm."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}

			if (Spec->HasTypedField<EJson::Number>(TEXT("landscape_quads_per_section")))
			{
				const int32 Q = Spec->GetIntegerField(TEXT("landscape_quads_per_section"));
				if (Q <= 0)
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("landscape_quads"));
					Issue->SetStringField(TEXT("message"), TEXT("landscape_quads_per_section must be positive when set."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}

			// Production pipeline metadata (height + material + paint layers)
			if (!Spec->HasTypedField<EJson::Number>(TEXT("cm_per_quad")))
			{
				bOk = false;
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("code"), TEXT("missing_cm_per_quad"));
				Issue->SetStringField(TEXT("message"), TEXT("terrain_spec.cm_per_quad is required."));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			}
			else
			{
				const double CmPerQuad = Spec->GetNumberField(TEXT("cm_per_quad"));
				if (CmPerQuad <= KINDA_SMALL_NUMBER)
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("cm_per_quad"));
					Issue->SetStringField(TEXT("message"), TEXT("terrain_spec.cm_per_quad must be positive."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}

			FString LandscapeMaterialInstancePath;
			if (!Spec->TryGetStringField(TEXT("landscape_material_instance_path"), LandscapeMaterialInstancePath) || LandscapeMaterialInstancePath.IsEmpty())
			{
				bOk = false;
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("code"), TEXT("missing_landscape_material_instance_path"));
				Issue->SetStringField(TEXT("message"), TEXT("terrain_spec.landscape_material_instance_path is required and must be a non-empty string."));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			}

			const TArray<TSharedPtr<FJsonValue>>* PaintLayers = nullptr;
			if (!Spec->TryGetArrayField(TEXT("paint_layers"), PaintLayers) || !PaintLayers || PaintLayers->Num() <= 0)
			{
				bOk = false;
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("code"), TEXT("missing_paint_layers"));
				Issue->SetStringField(TEXT("message"), TEXT("terrain_spec.paint_layers is required and must be a non-empty array."));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			}
			else
			{
				for (int32 i = 0; i < PaintLayers->Num(); ++i)
				{
					const TSharedPtr<FJsonValue>& V = (*PaintLayers)[i];
					const TSharedPtr<FJsonObject>* LayerObjPtr = nullptr;
					if (!V.IsValid() || !V->TryGetObject(LayerObjPtr) || !LayerObjPtr || !LayerObjPtr->IsValid())
					{
						bOk = false;
						TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("code"), TEXT("paint_layers_item_not_object"));
						Issue->SetStringField(TEXT("message"), TEXT("paint_layers[] items must be objects."));
						Issues.Add(MakeShared<FJsonValueObject>(Issue));
						continue;
					}

					FString LayerName;
					if (!(*LayerObjPtr)->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
					{
						bOk = false;
						TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("code"), TEXT("paint_layers_missing_layer_name"));
						Issue->SetStringField(TEXT("message"), TEXT("paint_layers[].layer_name is required and must be a non-empty string."));
						Issues.Add(MakeShared<FJsonValueObject>(Issue));
					}

					FString LayerInfoAssetPackagePath;
					if (!(*LayerObjPtr)->TryGetStringField(TEXT("layerinfo_asset_package_path"), LayerInfoAssetPackagePath) || LayerInfoAssetPackagePath.IsEmpty())
					{
						bOk = false;
						TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("code"), TEXT("paint_layers_missing_layerinfo_asset_package_path"));
						Issue->SetStringField(TEXT("message"), TEXT("paint_layers[].layerinfo_asset_package_path is required and must be a non-empty string."));
						Issues.Add(MakeShared<FJsonValueObject>(Issue));
					}
				}
			}

			// Height generation
			const TSharedPtr<FJsonObject>* HeightGen = nullptr;
			if (!Spec->TryGetObjectField(TEXT("height_generation"), HeightGen) || !HeightGen || !HeightGen->IsValid())
			{
				bOk = false;
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("code"), TEXT("missing_height_generation"));
				Issue->SetStringField(TEXT("message"), TEXT("terrain_spec.height_generation is required."));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			}
			else
			{
				FString Type;
				(*HeightGen)->TryGetStringField(TEXT("type"), Type); // optional, used for future extension

				if (!(*HeightGen)->HasTypedField<EJson::Number>(TEXT("seed")))
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("height_seed"));
					Issue->SetStringField(TEXT("message"), TEXT("height_generation.seed is required (int)."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
				if (!(*HeightGen)->HasTypedField<EJson::Number>(TEXT("frequency")))
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("height_frequency"));
					Issue->SetStringField(TEXT("message"), TEXT("height_generation.frequency is required (number)."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
				if (!(*HeightGen)->HasTypedField<EJson::Number>(TEXT("amplitude")))
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("height_amplitude"));
					Issue->SetStringField(TEXT("message"), TEXT("height_generation.amplitude is required (number)."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}

			// PCG fields are optional for validation unless present.
			FString PcgMode;
			if (Spec->TryGetStringField(TEXT("pcg_mode"), PcgMode) && !PcgMode.IsEmpty())
			{
				const FString ModeUpper = PcgMode.ToUpper();
				if (ModeUpper != TEXT("A") && ModeUpper != TEXT("B"))
				{
					bOk = false;
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("code"), TEXT("pcg_mode"));
					Issue->SetStringField(TEXT("message"), TEXT("terrain_spec.pcg_mode must be \"A\" or \"B\" when provided."));
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
				if (ModeUpper == TEXT("B"))
				{
					FString GraphAssetPath;
					if (!Spec->TryGetStringField(TEXT("pcg_graph_asset_path"), GraphAssetPath) || GraphAssetPath.IsEmpty())
					{
						bOk = false;
						TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("code"), TEXT("pcg_graph_asset_path"));
						Issue->SetStringField(TEXT("message"), TEXT("terrain_spec.pcg_graph_asset_path is required when pcg_mode is \"B\"."));
						Issues.Add(MakeShared<FJsonValueObject>(Issue));
					}
				}
			}

			OutStructured = MakeShared<FJsonObject>();
			OutStructured->SetBoolField(TEXT("valid"), bOk);
			OutStructured->SetArrayField(TEXT("issues"), Issues);
			OutStructured->SetStringField(TEXT("terrain_spec_version"), Spec->HasField(TEXT("version")) ? Spec->GetStringField(TEXT("version")) : TEXT("unspecified"));
			return true;
		}

		FString ContentBundleStatusToString(const EContentBundleStatus Status)
		{
			switch (Status)
			{
			case EContentBundleStatus::Registered:
				return TEXT("registered");
			case EContentBundleStatus::ReadyToInject:
				return TEXT("ready_to_inject");
			case EContentBundleStatus::FailedToInject:
				return TEXT("failed_to_inject");
			case EContentBundleStatus::ContentInjected:
				return TEXT("content_injected");
			case EContentBundleStatus::Unknown:
			default:
				return TEXT("unknown");
			}
		}

		TSharedRef<FJsonObject> ContentBundleEditorToJson(const FContentBundleEditor& ContentBundleEditor, const bool bIsEditing)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const UContentBundleDescriptor* Descriptor = ContentBundleEditor.GetDescriptor();
			Result->SetStringField(TEXT("displayName"), ContentBundleEditor.GetDisplayName());
			Result->SetStringField(TEXT("status"), ContentBundleStatusToString(ContentBundleEditor.GetStatus()));
			Result->SetBoolField(TEXT("isEditing"), bIsEditing || ContentBundleEditor.IsBeingEdited());
			Result->SetNumberField(TEXT("actorCount"), ContentBundleEditor.GetActorCount());
			Result->SetNumberField(TEXT("unsavedActorCount"), ContentBundleEditor.GetUnsavedActorAcount());
			Result->SetBoolField(TEXT("hasCookedContent"), ContentBundleEditor.HasCookedContent());
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			Result->SetBoolField(TEXT("hasContent"), ContentBundleEditor.HasContent());
#else
			// FContentBundleEditor::HasContent is 5.5+.
#endif
			Result->SetBoolField(TEXT("isValid"), ContentBundleEditor.IsValid());
			if (Descriptor)
			{
				Result->SetStringField(TEXT("guid"), Descriptor->GetGuid().ToString());
				Result->SetStringField(TEXT("descriptorDisplayName"), Descriptor->GetDisplayName());
				Result->SetStringField(TEXT("packageRoot"), Descriptor->GetPackageRoot());
				Result->SetObjectField(TEXT("descriptor"), FSololmcpEditorServices::MakeObjectReference(Descriptor));
			}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
			if (UActorDescContainerInstance* ContainerInstance = ContentBundleEditor.GetActorDescContainerInstance().Get())
			{
				Result->SetObjectField(TEXT("actorDescContainerInstance"), FSololmcpEditorServices::MakeObjectReference(ContainerInstance));
			}
#else
			// 5.4 replaced UActorDescContainer with UActorDescContainerInstance and
			// deprecated the old accessor. 5.3 still has the container itself, so the
			// field is reported from that rather than dropped.
			if (UActorDescContainer* Container = ContentBundleEditor.GetActorDescContainer().Get())
			{
				Result->SetObjectField(TEXT("actorDescContainerInstance"), FSololmcpEditorServices::MakeObjectReference(Container));
			}
#endif
			return Result;
		}

		TSharedPtr<FContentBundleEditor> ResolveContentBundleEditor(const TSharedRef<FJsonObject>& Arguments, FString& OutError)
		{
			UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
			if (!ContentBundleSubsystem)
			{
				OutError = TEXT("ContentBundleEditorSubsystem is unavailable.");
				return nullptr;
			}

			FString GuidString;
			if (Arguments->TryGetStringField(TEXT("guid"), GuidString) && !GuidString.IsEmpty())
			{
				FGuid Guid;
				if (!FGuid::Parse(GuidString, Guid))
				{
					OutError = TEXT("guid is not a valid GUID.");
					return nullptr;
				}
				if (TSharedPtr<FContentBundleEditor> ContentBundleEditor = ContentBundleSubsystem->GetEditorContentBundle(Guid))
				{
					return ContentBundleEditor;
				}
				OutError = TEXT("Content bundle was not found.");
				return nullptr;
			}

			FString DisplayName;
			if (Arguments->TryGetStringField(TEXT("display_name"), DisplayName) && !DisplayName.IsEmpty())
			{
				for (const TSharedPtr<FContentBundleEditor>& ContentBundleEditor : ContentBundleSubsystem->GetEditorContentBundles())
				{
					if (ContentBundleEditor.IsValid() && ContentBundleEditor->GetDisplayName().Equals(DisplayName, ESearchCase::IgnoreCase))
					{
						return ContentBundleEditor;
					}
				}
				OutError = TEXT("Content bundle was not found.");
				return nullptr;
			}

			OutError = TEXT("Missing content bundle guid or display_name.");
			return nullptr;
		}

#if SOMOLMCP_HAS_ACTORDESC_INSTANCE
		class FSololmcpStreamingGenerationErrorCollector final : public IStreamingGenerationErrorHandler
		{
		public:
			TArray<TSharedPtr<FJsonValue>> Findings;

			void AddSimpleFinding(const FString& Code, const FString& Message)
			{
				TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
				Finding->SetStringField(TEXT("code"), Code);
				Finding->SetStringField(TEXT("message"), Message);
				Findings.Add(MakeShared<FJsonValueObject>(Finding));
			}

			void AddActorFinding(const FString& Code, const FString& Message, const IWorldPartitionActorDescInstanceView& ActorDescView)
			{
				TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
				Finding->SetStringField(TEXT("code"), Code);
				Finding->SetStringField(TEXT("message"), Message);
				Finding->SetStringField(TEXT("actorName"), IStreamingGenerationErrorHandler::GetActorName(ActorDescView));
				Finding->SetStringField(TEXT("fullActorName"), IStreamingGenerationErrorHandler::GetFullActorName(ActorDescView));
				Findings.Add(MakeShared<FJsonValueObject>(Finding));
			}

			void AddActorPairFinding(const FString& Code, const FString& Message, const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView)
			{
				TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
				Finding->SetStringField(TEXT("code"), Code);
				Finding->SetStringField(TEXT("message"), Message);
				Finding->SetStringField(TEXT("actorName"), IStreamingGenerationErrorHandler::GetActorName(ActorDescView));
				Finding->SetStringField(TEXT("referenceActorName"), IStreamingGenerationErrorHandler::GetActorName(ReferenceActorDescView));
				Findings.Add(MakeShared<FJsonValueObject>(Finding));
			}

			virtual void OnInvalidRuntimeGrid(const IWorldPartitionActorDescInstanceView& ActorDescView, FName GridName) override
			{
				AddActorFinding(TEXT("invalid_runtime_grid"), FString::Printf(TEXT("Actor uses invalid runtime grid '%s'."), *GridName.ToString()), ActorDescView);
			}

			virtual void OnInvalidReference(const IWorldPartitionActorDescInstanceView& ActorDescView, const FGuid& ReferenceGuid, IWorldPartitionActorDescInstanceView* ReferenceActorDescView) override
			{
				const FString Message = ReferenceActorDescView
					? FString::Printf(TEXT("Actor has an invalid reference to '%s'."), *IStreamingGenerationErrorHandler::GetActorName(*ReferenceActorDescView))
					: FString::Printf(TEXT("Actor has an invalid reference '%s'."), *ReferenceGuid.ToString());
				AddActorFinding(TEXT("invalid_reference"), Message, ActorDescView);
			}

			virtual void OnInvalidReferenceGridPlacement(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
			{
				AddActorPairFinding(TEXT("invalid_reference_grid_placement"), TEXT("Actor references another actor using incompatible grid placement."), ActorDescView, ReferenceActorDescView);
			}

			virtual void OnInvalidReferenceDataLayers(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView, EDataLayerInvalidReason Reason) override
			{
				const FString ReasonString = Reason == EDataLayerInvalidReason::ReferencedActorDifferentExternalDataLayer
					? TEXT("different external data layer")
					: TEXT("different runtime data layers");
				AddActorPairFinding(TEXT("invalid_reference_data_layers"), FString::Printf(TEXT("Actor references another actor with %s."), *ReasonString), ActorDescView, ReferenceActorDescView);
			}

			virtual void OnInvalidReferenceRuntimeGrid(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
			{
				AddActorPairFinding(TEXT("invalid_reference_runtime_grid"), TEXT("Actor references another actor using a different runtime grid."), ActorDescView, ReferenceActorDescView);
			}

			#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			// 5.5+ only: the handler has no such callback on 5.4.
virtual void OnDataLayersLoadFilterMismatch(const IWorldPartitionActorDescInstanceView& ActorDescView) override
			{
				AddActorFinding(TEXT("data_layers_load_filter_mismatch"), TEXT("Actor mixes runtime data layers with incompatible load filters."), ActorDescView);
			}
#endif

			virtual void OnInvalidWorldReference(const IWorldPartitionActorDescInstanceView& ActorDescView, EWorldReferenceInvalidReason Reason) override
			{
				const FString ReasonString = Reason == EWorldReferenceInvalidReason::ReferencedActorHasDataLayers
					? TEXT("references a streamed actor with data layers")
					: TEXT("references a spatially loaded actor");
				AddActorFinding(TEXT("invalid_world_reference"), FString::Printf(TEXT("World reference is invalid because it %s."), *ReasonString), ActorDescView);
			}

			virtual void OnInvalidReferenceDataLayerAsset(const UDataLayerInstanceWithAsset* DataLayerInstance) override
			{
				AddSimpleFinding(TEXT("invalid_reference_data_layer_asset"), FString::Printf(TEXT("Data layer '%s' is missing a valid data layer asset."), DataLayerInstance ? *DataLayerInstance->GetDataLayerFullName() : TEXT("<null>")));
			}

			#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			// 5.5 added the EDataLayerHierarchyInvalidReason parameter; 5.4 takes two arguments and does not define the enum.
virtual void OnDataLayerHierarchyTypeMismatch(const UDataLayerInstance* DataLayerInstance, const UDataLayerInstance* Parent, EDataLayerHierarchyInvalidReason Reason) override
			{
				FString ReasonString = TEXT("incompatible data layer type");
				if (Reason == EDataLayerHierarchyInvalidReason::ClientOnlyDataLayerCantBeChild)
				{
					ReasonString = TEXT("client-only data layer cannot be a child");
				}
				else if (Reason == EDataLayerHierarchyInvalidReason::ServerOnlyDataLayerCantBeChild)
				{
					ReasonString = TEXT("server-only data layer cannot be a child");
				}

				AddSimpleFinding(TEXT("data_layer_hierarchy_type_mismatch"), FString::Printf(TEXT("Data layer '%s' is incompatible with parent '%s': %s."), DataLayerInstance ? *DataLayerInstance->GetDataLayerFullName() : TEXT("<null>"), Parent ? *Parent->GetDataLayerFullName() : TEXT("<null>"), *ReasonString));
			}
#else
			// 5.4 declares this with two parameters and has no
			// EDataLayerHierarchyInvalidReason, so the finding carries no reason.
			// Omitting the override entirely would leave the class abstract.
			virtual void OnDataLayerHierarchyTypeMismatch(const UDataLayerInstance* DataLayerInstance, const UDataLayerInstance* Parent) override
			{
				const FString DataLayerName = DataLayerInstance ? DataLayerInstance->GetDataLayerFullName() : TEXT("<null>");
				const FString ParentName = Parent ? Parent->GetDataLayerFullName() : TEXT("<null>");
				AddSimpleFinding(TEXT("data_layer_hierarchy_type_mismatch"), FString::Printf(TEXT("Data layer '%s' is incompatible with parent '%s'."), *DataLayerName, *ParentName));
			}
#endif

			#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			// 5.5+ only: the handler has no such callback on 5.4.
virtual void OnInvalidWorldDataLayersReference(const AWorldDataLayers*, const UDataLayerInstance* DataLayerInstance, const FText& Reason) override
			{
				AddSimpleFinding(TEXT("invalid_world_data_layers_reference"), FString::Printf(TEXT("WorldDataLayers reference is invalid for '%s': %s."), DataLayerInstance ? *DataLayerInstance->GetDataLayerFullName() : TEXT("<null>"), *Reason.ToString()));
			}
#endif

			virtual void OnDataLayerAssetConflict(const UDataLayerInstanceWithAsset* DataLayerInstance, const UDataLayerInstanceWithAsset* ConflictingDataLayerInstance) override
			{
				AddSimpleFinding(TEXT("data_layer_asset_conflict"), FString::Printf(TEXT("Data layer asset conflict between '%s' and '%s'."), DataLayerInstance ? *DataLayerInstance->GetDataLayerFullName() : TEXT("<null>"), ConflictingDataLayerInstance ? *ConflictingDataLayerInstance->GetDataLayerFullName() : TEXT("<null>")));
			}

			virtual void OnInvalidDataLayerAssetType(const UDataLayerInstanceWithAsset* DataLayerInstance, const UDataLayerAsset* DataLayerAsset) override
			{
				AddSimpleFinding(TEXT("invalid_data_layer_asset_type"), FString::Printf(TEXT("Data layer '%s' uses incompatible asset '%s'."), DataLayerInstance ? *DataLayerInstance->GetDataLayerFullName() : TEXT("<null>"), DataLayerAsset ? *DataLayerAsset->GetPathName() : TEXT("<null>")));
			}

			virtual void OnActorNeedsResave(const IWorldPartitionActorDescInstanceView& ActorDescView) override
			{
				AddActorFinding(TEXT("actor_needs_resave"), TEXT("Actor needs resave."), ActorDescView);
			}

			virtual void OnLevelInstanceInvalidWorldAsset(const IWorldPartitionActorDescInstanceView& ActorDescView, FName WorldAsset, ELevelInstanceInvalidReason Reason) override
			{
				// 5.5 renamed two of these: WorldAssetNotUsingExternalActors became
				// WorldAssetDontContainActorsMetadata, and the misspelled
				// WorldAssetImcompatiblePartitioned was corrected to Incompatible.
				// Same meanings, different spellings, so only the names are gated.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				const FString ReasonString = Reason == ELevelInstanceInvalidReason::WorldAssetNotFound ? TEXT("world asset not found")
					: Reason == ELevelInstanceInvalidReason::WorldAssetDontContainActorsMetadata ? TEXT("world asset does not contain actors metadata")
					: Reason == ELevelInstanceInvalidReason::WorldAssetIncompatiblePartitioned ? TEXT("world asset is partition incompatible")
#else
				const FString ReasonString = Reason == ELevelInstanceInvalidReason::WorldAssetNotFound ? TEXT("world asset not found")
					: Reason == ELevelInstanceInvalidReason::WorldAssetNotUsingExternalActors ? TEXT("world asset does not use external actors")
					: Reason == ELevelInstanceInvalidReason::WorldAssetImcompatiblePartitioned ? TEXT("world asset is partition incompatible")
#endif
					: Reason == ELevelInstanceInvalidReason::WorldAssetHasInvalidContainer ? TEXT("world asset has invalid container")
					: TEXT("circular reference");
				AddActorFinding(TEXT("level_instance_invalid_world_asset"), FString::Printf(TEXT("Level instance world asset '%s' is invalid: %s."), *WorldAsset.ToString(), *ReasonString), ActorDescView);
			}

			virtual void OnInvalidActorFilterReference(const IWorldPartitionActorDescInstanceView& ActorDescView, const IWorldPartitionActorDescInstanceView& ReferenceActorDescView) override
			{
				AddActorPairFinding(TEXT("invalid_actor_filter_reference"), TEXT("Actor references another actor with incompatible actor filters."), ActorDescView, ReferenceActorDescView);
			}

			virtual void OnInvalidHLODLayer(const IWorldPartitionActorDescInstanceView& ActorDescView) override
			{
				AddActorFinding(TEXT("invalid_hlod_layer"), TEXT("Actor has an invalid HLOD layer."), ActorDescView);
			}

			#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
			virtual void OnUnsupportedHLODLayer(const IWorldPartitionActorDescInstanceView& ActorDescView, const UHLODLayer* HLODLayer, EHLODLayerUnsupportedReason) override
			{
				const FString HLODLayerPath = HLODLayer ? HLODLayer->GetPathName() : FString(TEXT("<null>"));
				AddActorFinding(TEXT("unsupported_hlod_layer"), FString::Printf(TEXT("Actor uses unsupported HLOD layer '%s'."), *HLODLayerPath), ActorDescView);
			}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
			virtual void OnRejectedActorDescViewMutator(const IWorldPartitionActorDescInstanceView& ActorDescView, const FActorDescViewMutator&, EActorDescViewMutatorRejectedReason Reason) override
			{
				const FString ReasonString = Reason == EActorDescViewMutatorRejectedReason::DivergesClusterRuntimeGrid
					? TEXT("diverges cluster runtime grid")
					: TEXT("diverges cluster spatial loading");
				AddActorFinding(TEXT("rejected_actor_desc_view_mutator"), FString::Printf(TEXT("Actor descriptor mutator was rejected because it %s."), *ReasonString), ActorDescView);
			}
			#endif
#endif
		};
#else // SOMOLMCP_HAS_ACTORDESC_INSTANCE

		// UE 5.3 streaming-generation error handler. Same findings, different
		// interface: descriptor views are FWorldPartitionActorDescView, two extra
		// pure virtuals exist, and the data-layer callbacks carry no reason enum.
		class FSololmcpStreamingGenerationErrorCollector final : public IStreamingGenerationErrorHandler
		{
		public:
			TArray<TSharedPtr<FJsonValue>> Findings;

			void AddSimpleFinding(const FString& Code, const FString& Message)
			{
				TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
				Finding->SetStringField(TEXT("code"), Code);
				Finding->SetStringField(TEXT("message"), Message);
				Findings.Add(MakeShared<FJsonValueObject>(Finding));
			}

			void AddActorFinding(const FString& Code, const FString& Message, const FWorldPartitionActorDescView& ActorDescView)
			{
				TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
				Finding->SetStringField(TEXT("code"), Code);
				Finding->SetStringField(TEXT("message"), Message);
				Finding->SetStringField(TEXT("actorName"), IStreamingGenerationErrorHandler::GetActorName(ActorDescView));
				Finding->SetStringField(TEXT("fullActorName"), IStreamingGenerationErrorHandler::GetFullActorName(ActorDescView));
				Findings.Add(MakeShared<FJsonValueObject>(Finding));
			}

			void AddActorPairFinding(const FString& Code, const FString& Message, const FWorldPartitionActorDescView& ActorDescView, const FWorldPartitionActorDescView& ReferenceActorDescView)
			{
				TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
				Finding->SetStringField(TEXT("code"), Code);
				Finding->SetStringField(TEXT("message"), Message);
				Finding->SetStringField(TEXT("actorName"), IStreamingGenerationErrorHandler::GetActorName(ActorDescView));
				Finding->SetStringField(TEXT("referenceActorName"), IStreamingGenerationErrorHandler::GetActorName(ReferenceActorDescView));
				Findings.Add(MakeShared<FJsonValueObject>(Finding));
			}

			virtual void OnInvalidRuntimeGrid(const FWorldPartitionActorDescView& ActorDescView, FName GridName) override
			{
				AddActorFinding(TEXT("invalid_runtime_grid"), FString::Printf(TEXT("Actor uses invalid runtime grid '%s'."), *GridName.ToString()), ActorDescView);
			}

			virtual void OnInvalidReference(const FWorldPartitionActorDescView& ActorDescView, const FGuid& ReferenceGuid, FWorldPartitionActorDescView* ReferenceActorDescView) override
			{
				const FString Message = ReferenceActorDescView
					? FString::Printf(TEXT("Actor has an invalid reference to '%s'."), *IStreamingGenerationErrorHandler::GetActorName(*ReferenceActorDescView))
					: FString::Printf(TEXT("Actor has an invalid reference '%s'."), *ReferenceGuid.ToString());
				AddActorFinding(TEXT("invalid_reference"), Message, ActorDescView);
			}

			virtual void OnInvalidReferenceGridPlacement(const FWorldPartitionActorDescView& ActorDescView, const FWorldPartitionActorDescView& ReferenceActorDescView) override
			{
				AddActorPairFinding(TEXT("invalid_reference_grid_placement"), TEXT("Actor references another actor using incompatible grid placement."), ActorDescView, ReferenceActorDescView);
			}

			virtual void OnInvalidReferenceDataLayers(const FWorldPartitionActorDescView& ActorDescView, const FWorldPartitionActorDescView& ReferenceActorDescView) override
			{
				AddActorPairFinding(TEXT("invalid_reference_data_layers"), TEXT("Actor references another actor with different runtime data layers."), ActorDescView, ReferenceActorDescView);
			}

			virtual void OnInvalidReferenceRuntimeGrid(const FWorldPartitionActorDescView& ActorDescView, const FWorldPartitionActorDescView& ReferenceActorDescView) override
			{
				AddActorPairFinding(TEXT("invalid_reference_runtime_grid"), TEXT("Actor references another actor using a different runtime grid."), ActorDescView, ReferenceActorDescView);
			}

			virtual void OnInvalidReferenceLevelScriptStreamed(const FWorldPartitionActorDescView& ActorDescView) override
			{
				AddActorFinding(TEXT("invalid_reference_level_script_streamed"), TEXT("Level script references a streamed actor."), ActorDescView);
			}

			virtual void OnInvalidReferenceLevelScriptDataLayers(const FWorldPartitionActorDescView& ActorDescView) override
			{
				AddActorFinding(TEXT("invalid_reference_level_script_data_layers"), TEXT("Level script references an actor with data layers."), ActorDescView);
			}

			virtual void OnInvalidReferenceDataLayerAsset(const UDataLayerInstanceWithAsset* DataLayerInstance) override
			{
				AddSimpleFinding(TEXT("invalid_reference_data_layer_asset"), FString::Printf(TEXT("Data layer '%s' is missing a valid data layer asset."), DataLayerInstance ? *DataLayerInstance->GetDataLayerFullName() : TEXT("<null>")));
			}

			virtual void OnDataLayerHierarchyTypeMismatch(const UDataLayerInstance* DataLayerInstance, const UDataLayerInstance* Parent) override
			{
				AddSimpleFinding(TEXT("data_layer_hierarchy_type_mismatch"), FString::Printf(TEXT("Data layer '%s' is incompatible with parent '%s'."), DataLayerInstance ? *DataLayerInstance->GetDataLayerFullName() : TEXT("<null>"), Parent ? *Parent->GetDataLayerFullName() : TEXT("<null>")));
			}

			virtual void OnDataLayerAssetConflict(const UDataLayerInstanceWithAsset* DataLayerInstance, const UDataLayerInstanceWithAsset* ConflictingDataLayerInstance) override
			{
				AddSimpleFinding(TEXT("data_layer_asset_conflict"), FString::Printf(TEXT("Data layer asset conflict between '%s' and '%s'."), DataLayerInstance ? *DataLayerInstance->GetDataLayerFullName() : TEXT("<null>"), ConflictingDataLayerInstance ? *ConflictingDataLayerInstance->GetDataLayerFullName() : TEXT("<null>")));
			}

			virtual void OnActorNeedsResave(const FWorldPartitionActorDescView& ActorDescView) override
			{
				AddActorFinding(TEXT("actor_needs_resave"), TEXT("Actor needs resave."), ActorDescView);
			}

			virtual void OnLevelInstanceInvalidWorldAsset(const FWorldPartitionActorDescView& ActorDescView, FName WorldAsset, ELevelInstanceInvalidReason Reason) override
			{
				// Same 5.5 spelling correction as the collector above: this second
				// handler was left ungated, which is why it only surfaced once the
				// earlier errors in this header stopped masking it.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				const FString ReasonString = Reason == ELevelInstanceInvalidReason::WorldAssetNotFound ? TEXT("world asset not found")
					: Reason == ELevelInstanceInvalidReason::WorldAssetIncompatiblePartitioned ? TEXT("world asset is partition incompatible")
#else
				const FString ReasonString = Reason == ELevelInstanceInvalidReason::WorldAssetNotFound ? TEXT("world asset not found")
					: Reason == ELevelInstanceInvalidReason::WorldAssetImcompatiblePartitioned ? TEXT("world asset is partition incompatible")
#endif
					: Reason == ELevelInstanceInvalidReason::WorldAssetHasInvalidContainer ? TEXT("world asset has invalid container")
					: TEXT("circular reference");
				AddActorFinding(TEXT("level_instance_invalid_world_asset"), FString::Printf(TEXT("Level instance world asset '%s' is invalid: %s."), *WorldAsset.ToString(), *ReasonString), ActorDescView);
			}

			virtual void OnInvalidActorFilterReference(const FWorldPartitionActorDescView& ActorDescView, const FWorldPartitionActorDescView& ReferenceActorDescView) override
			{
				AddActorPairFinding(TEXT("invalid_actor_filter_reference"), TEXT("Actor references another actor with incompatible actor filters."), ActorDescView, ReferenceActorDescView);
			}
		};

#endif // SOMOLMCP_HAS_ACTORDESC_INSTANCE

		TSharedRef<FJsonObject> LinearColorToJson(const FLinearColor& Color)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("r"), Color.R);
			Result->SetNumberField(TEXT("g"), Color.G);
			Result->SetNumberField(TEXT("b"), Color.B);
			Result->SetNumberField(TEXT("a"), Color.A);
			return Result;
		}

		TSharedRef<FJsonObject> VectorToJson(const FVector& Vector)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("x"), Vector.X);
			Result->SetNumberField(TEXT("y"), Vector.Y);
			Result->SetNumberField(TEXT("z"), Vector.Z);
			return Result;
		}

		TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const FRotator Rotation = Transform.Rotator();
			TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
			RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
			RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
			RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
			Result->SetObjectField(TEXT("location"), VectorToJson(Transform.GetTranslation()));
			Result->SetObjectField(TEXT("rotation"), RotationJson);
			Result->SetObjectField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
			return Result;
		}

		TSharedRef<FJsonObject> BoxToJson(const FBox& Box)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("min"), VectorToJson(Box.Min));
			Result->SetObjectField(TEXT("max"), VectorToJson(Box.Max));
			return Result;
		}

		bool JsonToBox(const TSharedPtr<FJsonObject>& Object, FBox& OutBox)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			TSharedPtr<FJsonObject> MinObject;
			TSharedPtr<FJsonObject> MaxObject;
			if (!TryGetObjectField(Object.ToSharedRef(), TEXT("min"), MinObject) || !TryGetObjectField(Object.ToSharedRef(), TEXT("max"), MaxObject))
			{
				return false;
			}

			FVector Min;
			FVector Max;
			if (!FSololmcpEditorServices::JsonToVector(MinObject, Min) || !FSololmcpEditorServices::JsonToVector(MaxObject, Max))
			{
				return false;
			}

			OutBox = FBox(Min, Max);
			return true;
		}

		UNiagaraRendererProperties* FindNiagaraRendererByIndex(UNiagaraEmitter* Emitter, int32 RendererIndex)
		{
			if (!Emitter || !Emitter->GetLatestEmitterData())
			{
				return nullptr;
			}

			const TArray<UNiagaraRendererProperties*>& Renderers = Emitter->GetLatestEmitterData()->GetRenderers();
			return Renderers.IsValidIndex(RendererIndex) ? Renderers[RendererIndex] : nullptr;
		}

		TSharedRef<FJsonObject> NiagaraRendererToJson(UNiagaraRendererProperties* Renderer, int32 RendererIndex)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("index"), RendererIndex);
			Result->SetStringField(TEXT("class"), Renderer ? Renderer->GetClass()->GetPathName() : FString());
			Result->SetStringField(TEXT("name"), Renderer ? Renderer->GetName() : FString());
			Result->SetBoolField(TEXT("enabled"), Renderer ? Renderer->GetIsEnabled() : false);
			return Result;
		}

		TSharedRef<FJsonObject> NiagaraRenderersToJson(UNiagaraEmitter* Emitter)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> RenderersJson;
			if (Emitter && Emitter->GetLatestEmitterData())
			{
				const TArray<UNiagaraRendererProperties*>& Renderers = Emitter->GetLatestEmitterData()->GetRenderers();
				for (int32 Index = 0; Index < Renderers.Num(); ++Index)
				{
					RenderersJson.Add(MakeShared<FJsonValueObject>(NiagaraRendererToJson(Renderers[Index], Index)));
				}
			}
			Result->SetArrayField(TEXT("renderers"), RenderersJson);
			Result->SetNumberField(TEXT("count"), RenderersJson.Num());
			return Result;
		}

		TSharedRef<FJsonObject> CineCameraActorToJson(ACineCameraActor* Actor)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Actor)
			{
				return Result;
			}

			Result->SetObjectField(TEXT("actor"), FSololmcpEditorServices::MakeActorReference(Actor));

			UCineCameraComponent* CameraComponent = Actor->GetCineCameraComponent();
			if (CameraComponent)
			{
				TSharedRef<FJsonObject> CameraJson = MakeShared<FJsonObject>();
				CameraJson->SetNumberField(TEXT("currentFocalLength"), CameraComponent->CurrentFocalLength);
				CameraJson->SetNumberField(TEXT("currentAperture"), CameraComponent->CurrentAperture);
				CameraJson->SetNumberField(TEXT("currentFocusDistance"), CameraComponent->CurrentFocusDistance);
				CameraJson->SetStringField(TEXT("filmbackPreset"), CameraComponent->GetFilmbackPresetName());
				CameraJson->SetStringField(TEXT("lensPreset"), CameraComponent->GetLensPresetName());
				CameraJson->SetStringField(TEXT("cropPreset"), CameraComponent->GetCropPresetName());

				TSharedRef<FJsonObject> FilmbackJson = MakeShared<FJsonObject>();
				FilmbackJson->SetNumberField(TEXT("sensorWidth"), CameraComponent->Filmback.SensorWidth);
				FilmbackJson->SetNumberField(TEXT("sensorHeight"), CameraComponent->Filmback.SensorHeight);
	#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			FilmbackJson->SetNumberField(TEXT("sensorHorizontalOffset"), CameraComponent->Filmback.SensorHorizontalOffset);
#else
			// Sensor offsets arrived in 5.5; omit rather than report a wrong 0.
#endif
	#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			FilmbackJson->SetNumberField(TEXT("sensorVerticalOffset"), CameraComponent->Filmback.SensorVerticalOffset);
#else
			// Sensor offsets arrived in 5.5.
#endif
				FilmbackJson->SetNumberField(TEXT("sensorAspectRatio"), CameraComponent->Filmback.SensorAspectRatio);
				CameraJson->SetObjectField(TEXT("filmback"), FilmbackJson);

				TSharedRef<FJsonObject> LensJson = MakeShared<FJsonObject>();
				LensJson->SetNumberField(TEXT("minFocalLength"), CameraComponent->LensSettings.MinFocalLength);
				LensJson->SetNumberField(TEXT("maxFocalLength"), CameraComponent->LensSettings.MaxFocalLength);
				LensJson->SetNumberField(TEXT("minFStop"), CameraComponent->LensSettings.MinFStop);
				LensJson->SetNumberField(TEXT("maxFStop"), CameraComponent->LensSettings.MaxFStop);
				LensJson->SetNumberField(TEXT("minimumFocusDistance"), CameraComponent->LensSettings.MinimumFocusDistance);
				LensJson->SetNumberField(TEXT("squeezeFactor"), CameraComponent->LensSettings.SqueezeFactor);
				LensJson->SetNumberField(TEXT("diaphragmBladeCount"), CameraComponent->LensSettings.DiaphragmBladeCount);
				CameraJson->SetObjectField(TEXT("lens"), LensJson);

				TSharedRef<FJsonObject> FocusJson = MakeShared<FJsonObject>();
				FocusJson->SetStringField(TEXT("method"), StaticEnum<ECameraFocusMethod>()->GetNameStringByValue(static_cast<int64>(CameraComponent->FocusSettings.FocusMethod)));
				FocusJson->SetNumberField(TEXT("manualFocusDistance"), CameraComponent->FocusSettings.ManualFocusDistance);
				FocusJson->SetBoolField(TEXT("smoothFocusChanges"), CameraComponent->FocusSettings.bSmoothFocusChanges);
				FocusJson->SetNumberField(TEXT("focusSmoothingInterpSpeed"), CameraComponent->FocusSettings.FocusSmoothingInterpSpeed);
				FocusJson->SetNumberField(TEXT("focusOffset"), CameraComponent->FocusSettings.FocusOffset);
				CameraJson->SetObjectField(TEXT("focus"), FocusJson);

				Result->SetObjectField(TEXT("camera"), CameraJson);
			}

			TSharedRef<FJsonObject> LookAtJson = MakeShared<FJsonObject>();
			LookAtJson->SetBoolField(TEXT("enabled"), Actor->LookatTrackingSettings.bEnableLookAtTracking);
			LookAtJson->SetBoolField(TEXT("drawDebug"), Actor->LookatTrackingSettings.bDrawDebugLookAtTrackingPosition);
			LookAtJson->SetNumberField(TEXT("interpSpeed"), Actor->LookatTrackingSettings.LookAtTrackingInterpSpeed);
			LookAtJson->SetObjectField(TEXT("relativeOffset"), VectorToJson(Actor->LookatTrackingSettings.RelativeOffset));
			LookAtJson->SetBoolField(TEXT("allowRoll"), Actor->LookatTrackingSettings.bAllowRoll);
			if (AActor* TrackedActor = Actor->LookatTrackingSettings.ActorToTrack.Get())
			{
				LookAtJson->SetObjectField(TEXT("trackedActor"), FSololmcpEditorServices::MakeActorReference(TrackedActor));
			}
			Result->SetObjectField(TEXT("lookAt"), LookAtJson);

			return Result;
		}

		TSharedRef<FJsonObject> AnimationNotifyEventToJson(const FAnimNotifyEvent& Event)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("guid"), Event.Guid.ToString());
			Result->SetStringField(TEXT("notifyName"), Event.NotifyName.ToString());
			Result->SetNumberField(TEXT("time"), Event.GetTriggerTime());
			Result->SetNumberField(TEXT("duration"), Event.GetDuration());
			Result->SetNumberField(TEXT("trackIndex"), Event.TrackIndex);
			Result->SetBoolField(TEXT("isState"), Event.NotifyStateClass != nullptr);
			if (Event.Notify)
			{
				Result->SetStringField(TEXT("class"), Event.Notify->GetClass()->GetPathName());
			}
			else if (Event.NotifyStateClass)
			{
				Result->SetStringField(TEXT("class"), Event.NotifyStateClass->GetClass()->GetPathName());
			}
			return Result;
		}

		FAnimNotifyEvent* FindAnimationNotifyByGuid(UAnimSequenceBase* Animation, const FString& NotifyGuidString)
		{
			if (!Animation)
			{
				return nullptr;
			}

			FGuid NotifyGuid;
			if (!FGuid::Parse(NotifyGuidString, NotifyGuid))
			{
				return nullptr;
			}

			for (FAnimNotifyEvent& Event : Animation->Notifies)
			{
				if (Event.Guid == NotifyGuid)
				{
					return &Event;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> VectorCurveKeysToJson(const TArray<float>& Times, const TArray<FVector>& Values)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (int32 Index = 0; Index < FMath::Min(Times.Num(), Values.Num()); ++Index)
			{
				TSharedRef<FJsonObject> KeyObject = MakeShared<FJsonObject>();
				KeyObject->SetNumberField(TEXT("time"), Times[Index]);
				KeyObject->SetObjectField(TEXT("value"), VectorToJson(Values[Index]));
				Keys.Add(MakeShared<FJsonValueObject>(KeyObject));
			}
			Result->SetArrayField(TEXT("keys"), Keys);
			Result->SetNumberField(TEXT("count"), Keys.Num());
			return Result;
		}

		TSharedRef<FJsonObject> TransformCurveKeysToJson(const TArray<float>& Times, const TArray<FTransform>& Values)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (int32 Index = 0; Index < FMath::Min(Times.Num(), Values.Num()); ++Index)
			{
				TSharedRef<FJsonObject> KeyObject = MakeShared<FJsonObject>();
				KeyObject->SetNumberField(TEXT("time"), Times[Index]);
				KeyObject->SetObjectField(TEXT("value"), TransformToJson(Values[Index]));
				Keys.Add(MakeShared<FJsonValueObject>(KeyObject));
			}
			Result->SetArrayField(TEXT("keys"), Keys);
			Result->SetNumberField(TEXT("count"), Keys.Num());
			return Result;
		}

		TSharedRef<FJsonObject> AnimationNotifyEventsToJson(const TArray<FAnimNotifyEvent>& Events)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FAnimNotifyEvent& Event : Events)
			{
				Items.Add(MakeShared<FJsonValueObject>(AnimationNotifyEventToJson(Event)));
			}
			Result->SetArrayField(TEXT("events"), Items);
			Result->SetNumberField(TEXT("count"), Items.Num());
			return Result;
		}

		TSharedRef<FJsonObject> NamesToJson(const TArray<FName>& Names, const FString& FieldName)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FName& Name : Names)
			{
				Items.Add(MakeShared<FJsonValueString>(Name.ToString()));
			}
			Result->SetArrayField(FieldName, Items);
			Result->SetNumberField(TEXT("count"), Items.Num());
			return Result;
		}

		TSharedRef<FJsonObject> FloatCurveKeysToJson(const TArray<float>& Times, const TArray<float>& Values)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Keys;
			const int32 Count = FMath::Min(Times.Num(), Values.Num());
			for (int32 Index = 0; Index < Count; ++Index)
			{
				TSharedRef<FJsonObject> KeyJson = MakeShared<FJsonObject>();
				KeyJson->SetNumberField(TEXT("time"), Times[Index]);
				KeyJson->SetNumberField(TEXT("value"), Values[Index]);
				Keys.Add(MakeShared<FJsonValueObject>(KeyJson));
			}
			Result->SetArrayField(TEXT("keys"), Keys);
			Result->SetNumberField(TEXT("count"), Keys.Num());
			return Result;
		}

		UNiagaraSystem* ResolveNiagaraSystem(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
		{
			UNiagaraSystem* System = Cast<UNiagaraSystem>(Services.LoadAsset(AssetPath, OutError));
			if (!System)
			{
				OutError = TEXT("Asset is not a Niagara system.");
			}
			return System;
		}

		UNiagaraComponent* ResolveNiagaraComponent(FSololmcpEditorServices& Services, const FString& ActorId, FString& OutError)
		{
			AActor* Actor = Services.FindActorByLabelOrName(ActorId, OutError);
			if (!Actor)
			{
				return nullptr;
			}

			UNiagaraComponent* Component = Actor->FindComponentByClass<UNiagaraComponent>();
			if (!Component)
			{
				OutError = TEXT("Actor does not have a Niagara component.");
			}
			return Component;
		}

		FNiagaraEmitterHandle* FindEmitterHandleByNameOrId(UNiagaraSystem* System, const FString& EmitterId)
		{
			if (!System)
			{
				return nullptr;
			}

			FGuid ParsedGuid;
			const bool bHasGuid = FGuid::Parse(EmitterId, ParsedGuid);
			for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (Handle.GetName().ToString() == EmitterId || (bHasGuid && Handle.GetId() == ParsedGuid))
				{
					return &Handle;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> NiagaraEmittersToJson(UNiagaraSystem* System)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Emitters;
			if (System)
			{
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					TSharedRef<FJsonObject> HandleJson = MakeShared<FJsonObject>();
					HandleJson->SetStringField(TEXT("name"), Handle.GetName().ToString());
					HandleJson->SetStringField(TEXT("id"), Handle.GetId().ToString());
					HandleJson->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
					Emitters.Add(MakeShared<FJsonValueObject>(HandleJson));
				}
			}
			Result->SetArrayField(TEXT("emitters"), Emitters);
			Result->SetNumberField(TEXT("count"), Emitters.Num());
			return Result;
		}
	}

}
