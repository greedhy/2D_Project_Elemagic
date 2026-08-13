// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Copyright SOMOLAGENT. Round E Montage V2 scaffold + Round G executor.
//
// UPCGSettings_CharacterMontageAssign — a *real* PCG graph node for per-actor
// AnimMontage assignment, successor to the external `pcg_character_montage_decorate`
// post-generate tool. Living inside the PCG graph lets authors:
//   • See montage selection as a first-class step in the DAG (caching, hashing, diffs).
//   • Chain into downstream nodes (e.g. SpawnActor + custom Blueprint that reads the
//     `montage_path` attribute).
//   • Drive deterministic output from the point-level seed rather than relying on an
//     out-of-band RPC after Generate completes.
//
// STATUS: Round G — `ExecuteInternal` now performs real pool sampling, byte-aligned
// with the Rust reference at `client/src-tauri/src/pcg_montage_sampler.rs`. Pool
// validation order, cumulative-weight table construction, hash_combine seed
// derivation, MontageRng (SplitMix64 finalizer) and play-rate jitter clamps all
// mirror the Rust implementation exactly. See the .cpp for line-by-line annotations.
//
// See also:
//   - skills/pcg-knowledge-base/instructions.md §Montage V2
//   - docs/AI_TOOLKIT_PCG_ENHANCEMENT_V1.md #E5 Montage V2

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "UObject/SoftObjectPath.h"
#include "PCGSettings_CharacterMontageAssign.generated.h"

class UAnimMontage;

// ─────────────────────────────────────────────────────────────────────────────
// FSOMOLMCPMontagePoolEntry — one weighted entry in the montage pool.
// Exposed to Blueprint so the node's Details panel and external tooling (the
// pcg_character_montage_decorate JSON args, roundtrip) share an identical shape.
// Fields mirror Rust `MontagePoolEntry` 1:1 (pcg_montage_sampler.rs).
// ─────────────────────────────────────────────────────────────────────────────
USTRUCT(BlueprintType)
struct SOMOLMCP_API FSOMOLMCPMontagePoolEntry
{
	GENERATED_BODY()

	/** AnimMontage to assign. Soft-referenced so the graph doesn't force-load during edit-time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Montage", meta = (AllowedClasses = "/Script/Engine.AnimMontage"))
	FSoftObjectPath MontagePath;

	/** Relative weight; normalized across all entries. 0 or negative = skipped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Montage", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float Weight = 1.0f;

	/** Inclusive lower bound of play-rate jitter. Default 1.0 = no jitter when Min==Max. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Montage", meta = (ClampMin = "0.01", UIMin = "0.01"))
	float PlayRateMin = 1.0f;

	/** Inclusive upper bound of play-rate jitter. If < Min, Min is used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Montage", meta = (ClampMin = "0.01", UIMin = "0.01"))
	float PlayRateMax = 1.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// UPCGSettings_CharacterMontageAssign — the PCG graph node.
// ─────────────────────────────────────────────────────────────────────────────
UCLASS(BlueprintType, ClassGroup = (Procedural), meta = (DisplayName = "SOMOL Character Montage Assign"))
class SOMOLMCP_API UPCGSettings_CharacterMontageAssign : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPCGSettings_CharacterMontageAssign();

	// UPCGSettings overrides
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override;
	virtual FText GetDefaultNodeTitle() const override;
	virtual FText GetNodeTooltipText() const override;
	virtual EPCGSettingsType GetType() const override;
#endif

	// ─── Parameters (mirror pcg_character_montage_decorate JSON inputs) ────────
	/** Weighted pool of candidate montages. Empty pool → node emits a warning and passes through. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOMOL|Montage")
	TArray<FSOMOLMCPMontagePoolEntry> MontagePool;

	/**
	 * If true, only write `montage_path` + `play_rate` metadata attributes to each
	 * point; downstream Blueprint spawner reads them and calls Montage_Play itself.
	 * If false, the executor is expected to play the montage on the spawned actor's
	 * AnimInstance directly (via post-SpawnActor callback).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOMOL|Montage")
	bool bAssignOnly = true;

	/** Optional substring filter on actor/point tag name. Empty = no filter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOMOL|Montage")
	FString ActorNamePattern;

	/** Deterministic RNG seed. Same seed + same input points ⇒ identical assignment.
	 *  Named MontageSeed to avoid shadowing UPCGSettings::Seed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOMOL|Montage")
	int32 MontageSeed = 0;

	/** Hard cap on number of points decorated in one Execute. Prevents runaway on huge tiles.
	 *  Mirrors Rust `MAX_POINTS = 100_000` ceiling in pcg_montage_sampler.rs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOMOL|Montage", meta = (ClampMin = "1", UIMin = "1"))
	int32 MaxPointsDecorated = 4096;

	// ─── Attribute names (advanced) ────────────────────────────────────────────
	/** Metadata attribute name (FString) written per point. Default "montage_path"
	 *  matches the Rust assignment field name so frontend preview + UE output line
	 *  up byte-for-byte. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOMOL|Montage|Advanced")
	FName MontageAttributeName = FName(TEXT("montage_path"));

	/** Metadata attribute name (float) written per point. Default "play_rate". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOMOL|Montage|Advanced")
	FName PlayRateAttributeName = FName(TEXT("play_rate"));

protected:
	// UPCGSettings
	virtual FPCGElementPtr CreateElement() const override;
};

// ─────────────────────────────────────────────────────────────────────────────
// FPCGElement_CharacterMontageAssign — per-execution.
// Round G executor: byte-aligned with pcg_montage_sampler.rs.
// ─────────────────────────────────────────────────────────────────────────────
// On 5.3 IPCGElement::Initialize is pure virtual and this element does not override
// it, which makes the class abstract there. FSimplePCGElement is the engine's own
// concrete base that supplies that one implementation; from 5.4 the base itself
// provides it, so IPCGElement is used directly.
//
// Expressed as an alias rather than a conditional class declaration: UnrealHeaderTool
// does not evaluate these conditionals and reports the second declaration as a stray
// one, which fails the build before the compiler ever runs.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 4
using FSomolPCGElementBase = FSimplePCGElement;
#else
using FSomolPCGElementBase = IPCGElement;
#endif

class FPCGElement_CharacterMontageAssign : public FSomolPCGElementBase
{
public:
	// IPCGElement
	virtual bool CanExecuteOnlyOnMainThread(FPCGContext* /*Context*/) const override { return false; }
	virtual bool IsCacheable(const UPCGSettings* /*InSettings*/) const override { return false; }

protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
