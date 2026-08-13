// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Tools/SololmcpToolRegistry.h"

#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace UE::SOMOLMCP
{
namespace BrushKernel
{
	struct FProfile
	{
		FString Id;
		FString DisplayName;
		double Radius = 100.0;
		double Strength = 1.0;
		double Spacing = 0.25;
		FString Falloff = TEXT("smooth");
		TArray<double> FalloffCurve;
		TArray<double> PressureCurve;
		bool bSymmetryX = false;
		bool bSymmetryY = false;
		bool bSymmetryZ = false;
		FString MaskAssetPath;
	};

	struct FFilter
	{
		FString Id;
		FString ProfileId;
		FString Type;
		bool bEnabled = true;
		double MinValue = 0.0;
		double MaxValue = 1.0;
	};

	struct FStroke
	{
		FString Id;
		FString ProfileId;
		FString Domain;
		FString TargetPath;
		FString ResourceLock;
		FString State = TEXT("recording");
		TArray<FVector> Points;
		FString WriterTool;
		FString ReceiptId;
		uint32 Revision = 1;
	};

	static FCriticalSection StateLock;
	static TMap<FString, FProfile> Profiles;
	static TMap<FString, FFilter> Filters;
	static TMap<FString, FStroke> Strokes;
	static TMap<FString, FString> Snapshots;

	static void SetFailure(
		TSharedRef<FJsonObject>& Out,
		FString& OutError,
		const FString& Code,
		const FString& Message,
		const FString& Status = TEXT("blocked"))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("status"), Status);
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("reason_code"), Code);
		Out->SetStringField(TEXT("message"), Message);
		Out->SetBoolField(TEXT("mutation_attempted"), false);
		OutError = Message;
	}

	static bool RequiredString(const TSharedRef<FJsonObject>& Args, const TCHAR* Field, FString& OutValue,
		TSharedRef<FJsonObject>& Out, FString& OutError)
	{
		if (!Args->TryGetStringField(Field, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
		{
			SetFailure(Out, OutError, TEXT("missing_required_parameter"),
				FString::Printf(TEXT("Missing or empty required parameter '%s'."), Field), TEXT("failed"));
			Out->SetStringField(TEXT("parameter"), Field);
			return false;
		}
		OutValue = OutValue.TrimStartAndEnd();
		return true;
	}

	static bool ReadPoint(const TSharedPtr<FJsonValue>& Value, FVector& OutPoint)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Point = Value->AsObject();
		if (!Point.IsValid() || !Point->HasTypedField<EJson::Number>(TEXT("x")) ||
			!Point->HasTypedField<EJson::Number>(TEXT("y")) || !Point->HasTypedField<EJson::Number>(TEXT("z")))
		{
			return false;
		}
		OutPoint = FVector(
			Point->GetNumberField(TEXT("x")),
			Point->GetNumberField(TEXT("y")),
			Point->GetNumberField(TEXT("z")));
		return !OutPoint.ContainsNaN();
	}

	static TSharedPtr<FJsonValue> PointJson(const FVector& Point)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Point.X);
		Object->SetNumberField(TEXT("y"), Point.Y);
		Object->SetNumberField(TEXT("z"), Point.Z);
		return MakeShared<FJsonValueObject>(Object);
	}

	static TArray<TSharedPtr<FJsonValue>> PointsJson(const TArray<FVector>& Points)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Points.Num());
		for (const FVector& Point : Points)
		{
			Values.Add(PointJson(Point));
		}
		return Values;
	}

	static TSharedRef<FJsonObject> PointSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("World X in centimeters."))},
				{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("World Y in centimeters."))},
				{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("World Z in centimeters."))},
			},
			{TEXT("x"), TEXT("y"), TEXT("z")}, TEXT("World-space point."), false);
	}

	static TSharedRef<FJsonObject> IdSchema(const TCHAR* Field, const FString& Description)
	{
		return FSololmcpSchemaBuilder::Object(
			{{Field, FSololmcpSchemaBuilder::String(Description, {}, 1, 128)}}, {Field}, FString(), false);
	}

	static FString NewId(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower());
	}

	static void WriteProfile(const FProfile& Profile, TSharedRef<FJsonObject>& Out)
	{
		Out->SetStringField(TEXT("profile_id"), Profile.Id);
		Out->SetStringField(TEXT("display_name"), Profile.DisplayName);
		Out->SetNumberField(TEXT("radius_cm"), Profile.Radius);
		Out->SetNumberField(TEXT("strength"), Profile.Strength);
		Out->SetNumberField(TEXT("spacing"), Profile.Spacing);
		Out->SetStringField(TEXT("falloff"), Profile.Falloff);
		Out->SetBoolField(TEXT("symmetry_x"), Profile.bSymmetryX);
		Out->SetBoolField(TEXT("symmetry_y"), Profile.bSymmetryY);
		Out->SetBoolField(TEXT("symmetry_z"), Profile.bSymmetryZ);
		Out->SetStringField(TEXT("mask_asset_path"), Profile.MaskAssetPath);
	}

	static void WriteStroke(const FStroke& Stroke, TSharedRef<FJsonObject>& Out, const bool bIncludePoints)
	{
		Out->SetStringField(TEXT("stroke_id"), Stroke.Id);
		Out->SetStringField(TEXT("profile_id"), Stroke.ProfileId);
		Out->SetStringField(TEXT("domain"), Stroke.Domain);
		Out->SetStringField(TEXT("target_path"), Stroke.TargetPath);
		Out->SetStringField(TEXT("resource_lock"), Stroke.ResourceLock);
		Out->SetStringField(TEXT("state"), Stroke.State);
		Out->SetNumberField(TEXT("revision"), Stroke.Revision);
		Out->SetNumberField(TEXT("point_count"), Stroke.Points.Num());
		if (bIncludePoints)
		{
			Out->SetArrayField(TEXT("points"), PointsJson(Stroke.Points));
		}
	}

	static FString StrokeDigest(const FStroke& Stroke)
	{
		FString Canonical = FString::Printf(TEXT("%s|%s|%s|%s|%u"), *Stroke.Id, *Stroke.ProfileId,
			*Stroke.Domain, *Stroke.TargetPath, Stroke.Revision);
		for (const FVector& Point : Stroke.Points)
		{
			Canonical += FString::Printf(TEXT("|%.4f,%.4f,%.4f"), Point.X, Point.Y, Point.Z);
		}
		return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Canonical));
	}

	static bool ValidateWriterReceipt(const FStroke& Stroke, const TSharedPtr<FJsonObject>& Receipt,
		TSharedRef<FJsonObject>& Out, FString& OutError)
	{
		if (!Receipt.IsValid())
		{
			SetFailure(Out, OutError, TEXT("missing_writer_receipt"),
				TEXT("A real domain-writer receipt is required before a brush stroke can be committed."));
			return false;
		}
		FString ReceiptId;
		FString TargetPath;
		FString Status;
		const bool bMutationApplied = Receipt->HasTypedField<EJson::Boolean>(TEXT("mutation_applied")) &&
			Receipt->GetBoolField(TEXT("mutation_applied"));
		const bool bReadbackVerified = Receipt->HasTypedField<EJson::Boolean>(TEXT("readback_verified")) &&
			Receipt->GetBoolField(TEXT("readback_verified"));
		Receipt->TryGetStringField(TEXT("receipt_id"), ReceiptId);
		Receipt->TryGetStringField(TEXT("target_path"), TargetPath);
		Receipt->TryGetStringField(TEXT("status"), Status);
		if (ReceiptId.IsEmpty() || !bMutationApplied || !bReadbackVerified ||
			!(Status == TEXT("succeeded") || Status == TEXT("completed")) || TargetPath != Stroke.TargetPath)
		{
			SetFailure(Out, OutError, TEXT("invalid_writer_receipt"),
				TEXT("Writer receipt must contain receipt_id, succeeded/completed status, mutation_applied=true, readback_verified=true, and the exact stroke target_path."));
			Out->SetStringField(TEXT("expected_target_path"), Stroke.TargetPath);
			Out->SetStringField(TEXT("received_target_path"), TargetPath);
			return false;
		}
		Out->SetStringField(TEXT("receipt_id"), ReceiptId);
		return true;
	}

	static TArray<double> ReadCurve(const TSharedRef<FJsonObject>& Args, const TCHAR* Field)
	{
		TArray<double> Curve;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Args->TryGetArrayField(Field, Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (Value.IsValid() && Value->Type == EJson::Number)
				{
					Curve.Add(FMath::Clamp(Value->AsNumber(), 0.0, 1.0));
				}
			}
		}
		return Curve;
	}
}

void RegisterBrushKernelTools(FSololmcpToolRegistry& Registry)
{
	using namespace BrushKernel;
	const TSharedRef<FJsonObject> CurveSchema = FSololmcpSchemaBuilder::Array(
		FSololmcpSchemaBuilder::Number(TEXT("Normalized sample."), 0.0, 1.0), TEXT("Normalized curve samples."), 2, 64);

	Registry.Register({TEXT("brush_profile_create"), TEXT("Create a shared native brush profile used by domain writers."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("profile_id"), FSololmcpSchemaBuilder::String(TEXT("Stable profile id; generated when omitted."))},
			{TEXT("display_name"), FSololmcpSchemaBuilder::String(TEXT("Human-readable name."))},
			{TEXT("radius_cm"), FSololmcpSchemaBuilder::Number(TEXT("Brush radius in centimeters."), 0.01, 1000000.0)},
			{TEXT("strength"), FSololmcpSchemaBuilder::Number(TEXT("Normalized strength."), 0.0, 1.0)},
			{TEXT("spacing"), FSololmcpSchemaBuilder::Number(TEXT("Normalized stamp spacing."), 0.001, 10.0)},
			{TEXT("falloff"), FSololmcpSchemaBuilder::String(TEXT("linear/smooth/spherical/tip/custom"), {TEXT("linear"), TEXT("smooth"), TEXT("spherical"), TEXT("tip"), TEXT("custom")})}},
			{}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FProfile Profile;
			Args->TryGetStringField(TEXT("profile_id"), Profile.Id);
			if (Profile.Id.IsEmpty()) Profile.Id = NewId(TEXT("brush"));
			Args->TryGetStringField(TEXT("display_name"), Profile.DisplayName);
			if (Profile.DisplayName.IsEmpty()) Profile.DisplayName = Profile.Id;
			if (Args->HasTypedField<EJson::Number>(TEXT("radius_cm"))) Profile.Radius = Args->GetNumberField(TEXT("radius_cm"));
			if (Args->HasTypedField<EJson::Number>(TEXT("strength"))) Profile.Strength = Args->GetNumberField(TEXT("strength"));
			if (Args->HasTypedField<EJson::Number>(TEXT("spacing"))) Profile.Spacing = Args->GetNumberField(TEXT("spacing"));
			Args->TryGetStringField(TEXT("falloff"), Profile.Falloff);
			if (Profile.Falloff.IsEmpty()) Profile.Falloff = TEXT("smooth");
			FScopeLock Lock(&StateLock);
			if (Profiles.Contains(Profile.Id))
			{
				SetFailure(Out, Error, TEXT("brush_profile_exists"), TEXT("A brush profile with this id already exists."), TEXT("failed"));
				return false;
			}
			Profiles.Add(Profile.Id, Profile);
			WriteProfile(Profile, Out);
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("created"));
			Summary = FString::Printf(TEXT("Created brush profile '%s'."), *Profile.Id);
			return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_profile_inspect"), TEXT("Inspect a shared brush profile and active-stroke usage."),
		IdSchema(TEXT("profile_id"), TEXT("Brush profile id.")),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("profile_id"), Id, Out, Error)) return false;
			FScopeLock Lock(&StateLock);
			const FProfile* Profile = Profiles.Find(Id);
			if (!Profile) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("Brush profile was not found."), TEXT("failed")); return false; }
			WriteProfile(*Profile, Out);
			int32 Active = 0; for (const TPair<FString, FStroke>& Pair : Strokes) if (Pair.Value.ProfileId == Id && Pair.Value.State == TEXT("recording")) ++Active;
			Out->SetNumberField(TEXT("active_strokes"), Active);
			Out->SetBoolField(TEXT("ok"), true);
			Summary = FString::Printf(TEXT("Brush profile '%s' has %d active strokes."), *Id, Active);
			return true;
		}, nullptr, 1});

	Registry.Register({TEXT("brush_profile_update"), TEXT("Update radius, strength, spacing, name, or falloff on a shared brush profile."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("profile_id"), FSololmcpSchemaBuilder::String(TEXT("Brush profile id."), {}, 1, 128)},
			{TEXT("display_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("radius_cm"), FSololmcpSchemaBuilder::Number(TEXT("Radius."), 0.01, 1000000.0)},
			{TEXT("strength"), FSololmcpSchemaBuilder::Number(TEXT("Strength."), 0.0, 1.0)},
			{TEXT("spacing"), FSololmcpSchemaBuilder::Number(TEXT("Spacing."), 0.001, 10.0)},
			{TEXT("falloff"), FSololmcpSchemaBuilder::String()}}, {TEXT("profile_id")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("profile_id"), Id, Out, Error)) return false;
			FScopeLock Lock(&StateLock); FProfile* Profile = Profiles.Find(Id);
			if (!Profile) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("Brush profile was not found."), TEXT("failed")); return false; }
			Args->TryGetStringField(TEXT("display_name"), Profile->DisplayName);
			if (Args->HasTypedField<EJson::Number>(TEXT("radius_cm"))) Profile->Radius = Args->GetNumberField(TEXT("radius_cm"));
			if (Args->HasTypedField<EJson::Number>(TEXT("strength"))) Profile->Strength = Args->GetNumberField(TEXT("strength"));
			if (Args->HasTypedField<EJson::Number>(TEXT("spacing"))) Profile->Spacing = Args->GetNumberField(TEXT("spacing"));
			FString Falloff; if (Args->TryGetStringField(TEXT("falloff"), Falloff) && !Falloff.IsEmpty()) Profile->Falloff = Falloff;
			WriteProfile(*Profile, Out); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("updated"));
			Summary = FString::Printf(TEXT("Updated brush profile '%s'."), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_profile_delete"), TEXT("Delete an unused shared brush profile; active strokes fail closed."),
		IdSchema(TEXT("profile_id"), TEXT("Brush profile id.")),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("profile_id"), Id, Out, Error)) return false;
			FScopeLock Lock(&StateLock);
			for (const TPair<FString, FStroke>& Pair : Strokes)
			{
				if (Pair.Value.ProfileId == Id && Pair.Value.State == TEXT("recording"))
				{ SetFailure(Out, Error, TEXT("brush_profile_in_use"), TEXT("Cancel or commit active strokes before deleting the profile.")); return false; }
			}
			if (Profiles.Remove(Id) == 0) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("Brush profile was not found."), TEXT("failed")); return false; }
			for (auto It = Filters.CreateIterator(); It; ++It) if (It.Value().ProfileId == Id) It.RemoveCurrent();
			Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("deleted")); Out->SetStringField(TEXT("profile_id"), Id);
			Summary = FString::Printf(TEXT("Deleted brush profile '%s'."), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_stroke_begin"), TEXT("Begin a target-bound brush stroke. This records intent and does not mutate the target asset."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("stroke_id"), FSololmcpSchemaBuilder::String(TEXT("Stable stroke id; generated when omitted."))},
			{TEXT("profile_id"), FSololmcpSchemaBuilder::String(TEXT("Existing brush profile."))},
			{TEXT("domain"), FSololmcpSchemaBuilder::String(TEXT("foliage/mesh_paint/modeling/fracture/terrain"), {TEXT("foliage"), TEXT("mesh_paint"), TEXT("modeling"), TEXT("fracture"), TEXT("terrain")})},
			{TEXT("target_path"), FSololmcpSchemaBuilder::String(TEXT("Exact actor or asset target."))},
			{TEXT("resource_lock"), FSololmcpSchemaBuilder::String(TEXT("Scheduler lock identity."))}},
			{TEXT("profile_id"), TEXT("domain"), TEXT("target_path"), TEXT("resource_lock")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FStroke Stroke;
			Args->TryGetStringField(TEXT("stroke_id"), Stroke.Id); if (Stroke.Id.IsEmpty()) Stroke.Id = NewId(TEXT("stroke"));
			if (!RequiredString(Args, TEXT("profile_id"), Stroke.ProfileId, Out, Error) ||
				!RequiredString(Args, TEXT("domain"), Stroke.Domain, Out, Error) ||
				!RequiredString(Args, TEXT("target_path"), Stroke.TargetPath, Out, Error) ||
				!RequiredString(Args, TEXT("resource_lock"), Stroke.ResourceLock, Out, Error)) return false;
			FScopeLock Lock(&StateLock);
			if (!Profiles.Contains(Stroke.ProfileId)) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("The selected brush profile does not exist."), TEXT("failed")); return false; }
			if (Strokes.Contains(Stroke.Id)) { SetFailure(Out, Error, TEXT("brush_stroke_exists"), TEXT("A stroke with this id already exists."), TEXT("failed")); return false; }
			Strokes.Add(Stroke.Id, Stroke); WriteStroke(Stroke, Out, false);
			Out->SetBoolField(TEXT("ok"), true); Out->SetBoolField(TEXT("mutation_attempted"), false); Out->SetStringField(TEXT("status"), TEXT("recording"));
			Summary = FString::Printf(TEXT("Started brush stroke '%s' for %s."), *Stroke.Id, *Stroke.TargetPath); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_stroke_append_points"), TEXT("Append validated world-space points to a recording brush stroke."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("stroke_id"), FSololmcpSchemaBuilder::String(TEXT("Stroke id."))},
			{TEXT("points"), FSololmcpSchemaBuilder::Array(PointSchema(), TEXT("Ordered stroke points."), 1, 8192)}},
			{TEXT("stroke_id"), TEXT("points")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("stroke_id"), Id, Out, Error)) return false;
			const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
			if (!Args->TryGetArrayField(TEXT("points"), Raw) || !Raw || Raw->IsEmpty()) { SetFailure(Out, Error, TEXT("invalid_brush_points"), TEXT("points must be a non-empty array."), TEXT("failed")); return false; }
			TArray<FVector> Parsed; Parsed.Reserve(Raw->Num());
			for (const TSharedPtr<FJsonValue>& Value : *Raw) { FVector Point; if (!ReadPoint(Value, Point)) { SetFailure(Out, Error, TEXT("invalid_brush_point"), TEXT("Every point must contain finite numeric x/y/z values."), TEXT("failed")); return false; } Parsed.Add(Point); }
			FScopeLock Lock(&StateLock); FStroke* Stroke = Strokes.Find(Id);
			if (!Stroke) { SetFailure(Out, Error, TEXT("brush_stroke_not_found"), TEXT("Brush stroke was not found."), TEXT("failed")); return false; }
			if (Stroke->State != TEXT("recording")) { SetFailure(Out, Error, TEXT("brush_stroke_not_recording"), TEXT("Only recording strokes accept points.")); return false; }
			Stroke->Points.Append(Parsed); ++Stroke->Revision; WriteStroke(*Stroke, Out, false);
			Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("recording")); Out->SetNumberField(TEXT("appended"), Parsed.Num());
			Summary = FString::Printf(TEXT("Appended %d points to stroke '%s'."), Parsed.Num(), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_stroke_preview"), TEXT("Read a bounded preview of a brush stroke without mutating UE state."),
		IdSchema(TEXT("stroke_id"), TEXT("Stroke id.")),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("stroke_id"), Id, Out, Error)) return false;
			FScopeLock Lock(&StateLock); const FStroke* Stroke = Strokes.Find(Id);
			if (!Stroke) { SetFailure(Out, Error, TEXT("brush_stroke_not_found"), TEXT("Brush stroke was not found."), TEXT("failed")); return false; }
			WriteStroke(*Stroke, Out, true); Out->SetStringField(TEXT("state_hash"), StrokeDigest(*Stroke)); Out->SetBoolField(TEXT("ok"), true); Out->SetBoolField(TEXT("read_only"), true);
			Summary = FString::Printf(TEXT("Previewed %d points for stroke '%s'."), Stroke->Points.Num(), *Id); return true;
		}, nullptr, 1});

	const TSharedRef<FJsonObject> ReceiptSchema = FSololmcpSchemaBuilder::Object({
		{TEXT("receipt_id"), FSololmcpSchemaBuilder::String()}, {TEXT("status"), FSololmcpSchemaBuilder::String()},
		{TEXT("target_path"), FSololmcpSchemaBuilder::String()}, {TEXT("mutation_applied"), FSololmcpSchemaBuilder::Boolean()},
		{TEXT("readback_verified"), FSololmcpSchemaBuilder::Boolean()}},
		{TEXT("receipt_id"), TEXT("status"), TEXT("target_path"), TEXT("mutation_applied"), TEXT("readback_verified")}, FString(), true);

	Registry.Register({TEXT("brush_stroke_commit"), TEXT("Finalize a stroke only after a domain writer supplies a successful mutation and readback receipt."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("stroke_id"), FSololmcpSchemaBuilder::String()}, {TEXT("writer_tool"), FSololmcpSchemaBuilder::String()},
			{TEXT("writer_receipt"), ReceiptSchema}}, {TEXT("stroke_id"), TEXT("writer_tool"), TEXT("writer_receipt")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id, Writer; if (!RequiredString(Args, TEXT("stroke_id"), Id, Out, Error) || !RequiredString(Args, TEXT("writer_tool"), Writer, Out, Error)) return false;
			const TSharedPtr<FJsonObject> Receipt = Args->HasTypedField<EJson::Object>(TEXT("writer_receipt")) ? Args->GetObjectField(TEXT("writer_receipt")) : nullptr;
			FScopeLock Lock(&StateLock); FStroke* Stroke = Strokes.Find(Id);
			if (!Stroke) { SetFailure(Out, Error, TEXT("brush_stroke_not_found"), TEXT("Brush stroke was not found."), TEXT("failed")); return false; }
			if (Stroke->State != TEXT("recording")) { SetFailure(Out, Error, TEXT("brush_stroke_not_recording"), TEXT("Stroke has already been finalized.")); return false; }
			if (Stroke->Points.IsEmpty()) { SetFailure(Out, Error, TEXT("brush_stroke_empty"), TEXT("Cannot commit an empty stroke.")); return false; }
			if (!ValidateWriterReceipt(*Stroke, Receipt, Out, Error)) return false;
			Stroke->WriterTool = Writer; Receipt->TryGetStringField(TEXT("receipt_id"), Stroke->ReceiptId); Stroke->State = TEXT("committed"); ++Stroke->Revision;
			WriteStroke(*Stroke, Out, false); Out->SetStringField(TEXT("writer_tool"), Writer); Out->SetStringField(TEXT("receipt_id"), Stroke->ReceiptId);
			Out->SetBoolField(TEXT("ok"), true); Out->SetBoolField(TEXT("production_complete"), true); Out->SetStringField(TEXT("status"), TEXT("committed"));
			Summary = FString::Printf(TEXT("Committed stroke '%s' after writer receipt '%s'."), *Id, *Stroke->ReceiptId); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_stroke_cancel"), TEXT("Cancel a recording brush stroke without touching its target."),
		IdSchema(TEXT("stroke_id"), TEXT("Stroke id.")),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("stroke_id"), Id, Out, Error)) return false;
			FScopeLock Lock(&StateLock); FStroke* Stroke = Strokes.Find(Id);
			if (!Stroke) { SetFailure(Out, Error, TEXT("brush_stroke_not_found"), TEXT("Brush stroke was not found."), TEXT("failed")); return false; }
			if (Stroke->State == TEXT("committed")) { SetFailure(Out, Error, TEXT("committed_stroke_cannot_cancel"), TEXT("Committed strokes require the domain writer rollback path.")); return false; }
			Stroke->State = TEXT("cancelled"); ++Stroke->Revision; WriteStroke(*Stroke, Out, false); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("cancelled"));
			Summary = FString::Printf(TEXT("Cancelled stroke '%s'."), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_surface_project"), TEXT("Project recording stroke points onto real editor-world collision using native line traces."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("stroke_id"), FSololmcpSchemaBuilder::String()}, {TEXT("direction"), PointSchema()},
			{TEXT("trace_distance_cm"), FSololmcpSchemaBuilder::Number(TEXT("Trace distance."), 1.0, 10000000.0)}},
			{TEXT("stroke_id"), TEXT("direction")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("stroke_id"), Id, Out, Error)) return false;
			FVector Direction; if (!Args->HasTypedField<EJson::Object>(TEXT("direction")) || !ReadPoint(MakeShared<FJsonValueObject>(Args->GetObjectField(TEXT("direction"))), Direction) || Direction.IsNearlyZero())
			{ SetFailure(Out, Error, TEXT("invalid_projection_direction"), TEXT("direction must be a non-zero finite x/y/z vector."), TEXT("failed")); return false; }
			const double Distance = Args->HasTypedField<EJson::Number>(TEXT("trace_distance_cm")) ? Args->GetNumberField(TEXT("trace_distance_cm")) : 100000.0;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { SetFailure(Out, Error, TEXT("editor_world_unavailable"), TEXT("No editor world is available for projection.")); return false; }
			FScopeLock Lock(&StateLock); FStroke* Stroke = Strokes.Find(Id);
			if (!Stroke) { SetFailure(Out, Error, TEXT("brush_stroke_not_found"), TEXT("Brush stroke was not found."), TEXT("failed")); return false; }
			if (Stroke->State != TEXT("recording") || Stroke->Points.IsEmpty()) { SetFailure(Out, Error, TEXT("brush_stroke_not_projectable"), TEXT("Projection requires a non-empty recording stroke.")); return false; }
			TArray<FVector> Projected = Stroke->Points; int32 HitCount = 0; const FVector Ray = Direction.GetSafeNormal() * Distance;
			FCollisionQueryParams Query(SCENE_QUERY_STAT(SOMOLMCPBrushSurfaceProject), true);
			for (int32 Index = 0; Index < Projected.Num(); ++Index)
			{
				FHitResult Hit; if (World->LineTraceSingleByChannel(Hit, Projected[Index], Projected[Index] + Ray, ECC_Visibility, Query)) { Projected[Index] = Hit.ImpactPoint; ++HitCount; }
			}
			if (HitCount == 0) { SetFailure(Out, Error, TEXT("brush_projection_no_hits"), TEXT("Projection produced no collision hits; stroke was left unchanged.")); return false; }
			Stroke->Points = MoveTemp(Projected); ++Stroke->Revision; WriteStroke(*Stroke, Out, false); Out->SetNumberField(TEXT("projected_points"), HitCount); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("projected"));
			Summary = FString::Printf(TEXT("Projected %d/%d points for stroke '%s'."), HitCount, Stroke->Points.Num(), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_filter_create"), TEXT("Create a typed range filter attached to a brush profile."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("filter_id"), FSololmcpSchemaBuilder::String(TEXT("Generated when omitted."))}, {TEXT("profile_id"), FSololmcpSchemaBuilder::String()},
			{TEXT("filter_type"), FSololmcpSchemaBuilder::String(TEXT("slope/height/material/tag/normal/distance"), {TEXT("slope"), TEXT("height"), TEXT("material"), TEXT("tag"), TEXT("normal"), TEXT("distance")})},
			{TEXT("min_value"), FSololmcpSchemaBuilder::Number()}, {TEXT("max_value"), FSololmcpSchemaBuilder::Number()}},
			{TEXT("profile_id"), TEXT("filter_type")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FFilter Filter; Args->TryGetStringField(TEXT("filter_id"), Filter.Id); if (Filter.Id.IsEmpty()) Filter.Id = NewId(TEXT("filter"));
			if (!RequiredString(Args, TEXT("profile_id"), Filter.ProfileId, Out, Error) || !RequiredString(Args, TEXT("filter_type"), Filter.Type, Out, Error)) return false;
			if (Args->HasTypedField<EJson::Number>(TEXT("min_value"))) Filter.MinValue = Args->GetNumberField(TEXT("min_value"));
			if (Args->HasTypedField<EJson::Number>(TEXT("max_value"))) Filter.MaxValue = Args->GetNumberField(TEXT("max_value"));
			if (Filter.MinValue > Filter.MaxValue) { SetFailure(Out, Error, TEXT("invalid_filter_range"), TEXT("min_value cannot exceed max_value."), TEXT("failed")); return false; }
			FScopeLock Lock(&StateLock); if (!Profiles.Contains(Filter.ProfileId)) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("Brush profile was not found."), TEXT("failed")); return false; }
			if (Filters.Contains(Filter.Id)) { SetFailure(Out, Error, TEXT("brush_filter_exists"), TEXT("A filter with this id already exists."), TEXT("failed")); return false; }
			Filters.Add(Filter.Id, Filter); Out->SetStringField(TEXT("filter_id"), Filter.Id); Out->SetStringField(TEXT("profile_id"), Filter.ProfileId); Out->SetStringField(TEXT("filter_type"), Filter.Type); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("created"));
			Summary = FString::Printf(TEXT("Created brush filter '%s'."), *Filter.Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_filter_update"), TEXT("Update range or enabled state on a shared brush filter."),
		FSololmcpSchemaBuilder::Object({{TEXT("filter_id"), FSololmcpSchemaBuilder::String()}, {TEXT("enabled"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("min_value"), FSololmcpSchemaBuilder::Number()}, {TEXT("max_value"), FSololmcpSchemaBuilder::Number()}}, {TEXT("filter_id")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("filter_id"), Id, Out, Error)) return false; FScopeLock Lock(&StateLock); FFilter* Filter = Filters.Find(Id);
			if (!Filter) { SetFailure(Out, Error, TEXT("brush_filter_not_found"), TEXT("Brush filter was not found."), TEXT("failed")); return false; }
			if (Args->HasTypedField<EJson::Boolean>(TEXT("enabled"))) Filter->bEnabled = Args->GetBoolField(TEXT("enabled"));
			if (Args->HasTypedField<EJson::Number>(TEXT("min_value"))) Filter->MinValue = Args->GetNumberField(TEXT("min_value"));
			if (Args->HasTypedField<EJson::Number>(TEXT("max_value"))) Filter->MaxValue = Args->GetNumberField(TEXT("max_value"));
			if (Filter->MinValue > Filter->MaxValue) { SetFailure(Out, Error, TEXT("invalid_filter_range"), TEXT("min_value cannot exceed max_value."), TEXT("failed")); return false; }
			Out->SetStringField(TEXT("filter_id"), Id); Out->SetBoolField(TEXT("enabled"), Filter->bEnabled); Out->SetNumberField(TEXT("min_value"), Filter->MinValue); Out->SetNumberField(TEXT("max_value"), Filter->MaxValue); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("updated"));
			Summary = FString::Printf(TEXT("Updated brush filter '%s'."), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_falloff_curve_set"), TEXT("Set normalized falloff samples on a shared brush profile."),
		FSololmcpSchemaBuilder::Object({{TEXT("profile_id"), FSololmcpSchemaBuilder::String()}, {TEXT("samples"), CurveSchema}}, {TEXT("profile_id"), TEXT("samples")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("profile_id"), Id, Out, Error)) return false; TArray<double> Curve = ReadCurve(Args, TEXT("samples"));
			if (Curve.Num() < 2) { SetFailure(Out, Error, TEXT("invalid_falloff_curve"), TEXT("At least two numeric samples are required."), TEXT("failed")); return false; }
			FScopeLock Lock(&StateLock); FProfile* Profile = Profiles.Find(Id); if (!Profile) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("Brush profile was not found."), TEXT("failed")); return false; }
			Profile->FalloffCurve = MoveTemp(Curve); Profile->Falloff = TEXT("custom"); Out->SetStringField(TEXT("profile_id"), Id); Out->SetNumberField(TEXT("sample_count"), Profile->FalloffCurve.Num()); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("updated"));
			Summary = FString::Printf(TEXT("Set %d falloff samples on '%s'."), Profile->FalloffCurve.Num(), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_pressure_curve_set"), TEXT("Set normalized pressure-to-strength samples on a shared brush profile."),
		FSololmcpSchemaBuilder::Object({{TEXT("profile_id"), FSololmcpSchemaBuilder::String()}, {TEXT("samples"), CurveSchema}}, {TEXT("profile_id"), TEXT("samples")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("profile_id"), Id, Out, Error)) return false; TArray<double> Curve = ReadCurve(Args, TEXT("samples"));
			if (Curve.Num() < 2) { SetFailure(Out, Error, TEXT("invalid_pressure_curve"), TEXT("At least two numeric samples are required."), TEXT("failed")); return false; }
			FScopeLock Lock(&StateLock); FProfile* Profile = Profiles.Find(Id); if (!Profile) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("Brush profile was not found."), TEXT("failed")); return false; }
			Profile->PressureCurve = MoveTemp(Curve); Out->SetStringField(TEXT("profile_id"), Id); Out->SetNumberField(TEXT("sample_count"), Profile->PressureCurve.Num()); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("updated"));
			Summary = FString::Printf(TEXT("Set %d pressure samples on '%s'."), Profile->PressureCurve.Num(), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_symmetry_set"), TEXT("Set X/Y/Z symmetry flags on a shared brush profile."),
		FSololmcpSchemaBuilder::Object({{TEXT("profile_id"), FSololmcpSchemaBuilder::String()}, {TEXT("x"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("y"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("z"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("profile_id")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("profile_id"), Id, Out, Error)) return false; FScopeLock Lock(&StateLock); FProfile* Profile = Profiles.Find(Id);
			if (!Profile) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("Brush profile was not found."), TEXT("failed")); return false; }
			if (Args->HasTypedField<EJson::Boolean>(TEXT("x"))) Profile->bSymmetryX = Args->GetBoolField(TEXT("x"));
			if (Args->HasTypedField<EJson::Boolean>(TEXT("y"))) Profile->bSymmetryY = Args->GetBoolField(TEXT("y"));
			if (Args->HasTypedField<EJson::Boolean>(TEXT("z"))) Profile->bSymmetryZ = Args->GetBoolField(TEXT("z"));
			WriteProfile(*Profile, Out); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("updated")); Summary = FString::Printf(TEXT("Updated symmetry for '%s'."), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_mask_bind"), TEXT("Bind or clear a /Game mask asset path on a brush profile."),
		FSololmcpSchemaBuilder::Object({{TEXT("profile_id"), FSololmcpSchemaBuilder::String()}, {TEXT("mask_asset_path"), FSololmcpSchemaBuilder::String(TEXT("/Game path; empty clears the mask."))}}, {TEXT("profile_id"), TEXT("mask_asset_path")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id, Path; if (!RequiredString(Args, TEXT("profile_id"), Id, Out, Error) || !Args->TryGetStringField(TEXT("mask_asset_path"), Path)) { SetFailure(Out, Error, TEXT("missing_required_parameter"), TEXT("mask_asset_path is required; pass an empty string to clear it."), TEXT("failed")); return false; }
			if (!Path.IsEmpty() && !Path.StartsWith(TEXT("/Game/"))) { SetFailure(Out, Error, TEXT("invalid_mask_asset_path"), TEXT("Mask path must be empty or start with /Game/."), TEXT("failed")); return false; }
			FScopeLock Lock(&StateLock); FProfile* Profile = Profiles.Find(Id); if (!Profile) { SetFailure(Out, Error, TEXT("brush_profile_not_found"), TEXT("Brush profile was not found."), TEXT("failed")); return false; }
			Profile->MaskAssetPath = Path; WriteProfile(*Profile, Out); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), Path.IsEmpty() ? TEXT("cleared") : TEXT("bound")); Summary = FString::Printf(TEXT("%s mask for '%s'."), Path.IsEmpty() ? TEXT("Cleared") : TEXT("Bound"), *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_operation_snapshot"), TEXT("Create an immutable state hash snapshot for a brush stroke before a domain write."),
		IdSchema(TEXT("stroke_id"), TEXT("Stroke id.")),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("stroke_id"), Id, Out, Error)) return false; FScopeLock Lock(&StateLock); const FStroke* Stroke = Strokes.Find(Id);
			if (!Stroke) { SetFailure(Out, Error, TEXT("brush_stroke_not_found"), TEXT("Brush stroke was not found."), TEXT("failed")); return false; }
			const FString SnapshotId = NewId(TEXT("brush_snapshot")); const FString Digest = StrokeDigest(*Stroke); Snapshots.Add(SnapshotId, Digest);
			Out->SetStringField(TEXT("snapshot_id"), SnapshotId); Out->SetStringField(TEXT("stroke_id"), Id); Out->SetStringField(TEXT("state_hash"), Digest); Out->SetNumberField(TEXT("revision"), Stroke->Revision); Out->SetBoolField(TEXT("ok"), true); Out->SetBoolField(TEXT("immutable"), true); Out->SetStringField(TEXT("status"), TEXT("captured"));
			Summary = FString::Printf(TEXT("Captured snapshot '%s' for stroke '%s'."), *SnapshotId, *Id); return true;
		}, nullptr, 0});

	Registry.Register({TEXT("brush_operation_receipt_validate"), TEXT("Validate that a domain writer really mutated and read back the exact brush target."),
		FSololmcpSchemaBuilder::Object({{TEXT("stroke_id"), FSololmcpSchemaBuilder::String()}, {TEXT("writer_receipt"), ReceiptSchema}}, {TEXT("stroke_id"), TEXT("writer_receipt")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString Id; if (!RequiredString(Args, TEXT("stroke_id"), Id, Out, Error)) return false;
			const TSharedPtr<FJsonObject> Receipt = Args->HasTypedField<EJson::Object>(TEXT("writer_receipt")) ? Args->GetObjectField(TEXT("writer_receipt")) : nullptr;
			FScopeLock Lock(&StateLock); const FStroke* Stroke = Strokes.Find(Id); if (!Stroke) { SetFailure(Out, Error, TEXT("brush_stroke_not_found"), TEXT("Brush stroke was not found."), TEXT("failed")); return false; }
			if (!ValidateWriterReceipt(*Stroke, Receipt, Out, Error)) return false;
			Out->SetStringField(TEXT("stroke_id"), Id); Out->SetStringField(TEXT("target_path"), Stroke->TargetPath); Out->SetBoolField(TEXT("ok"), true); Out->SetBoolField(TEXT("receipt_valid"), true); Out->SetStringField(TEXT("status"), TEXT("validated"));
			Summary = FString::Printf(TEXT("Validated writer receipt for stroke '%s'."), *Id); return true;
		}, nullptr, 0});
}
}
