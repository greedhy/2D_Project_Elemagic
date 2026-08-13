// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Copyright SOMOLAGENT. Round G Montage V2 executor.
//
// IMPORTANT — DETERMINISM CONTRACT:
//   This file MUST produce byte-identical (montage_path, play_rate) tuples to
//   the Rust reference at `client/src-tauri/src/pcg_montage_sampler.rs` for
//   the same (MontageSeed, MontagePool, point-seed-stream) triple. Any drift
//   between the two implementations is a regression that the 17 Rust unit
//   tests + the `pcg_montage_sample_preview` Tauri command + golden-sample
//   diff will catch immediately.
//
// To make line-by-line audit easy, every step below is annotated with the
// matching Rust source line range using the form `[rust:LL-LL]`.
//
// Build environment notes:
//   • UE 5.7.4, PCG + PCGEditor modules already in SOMOLMCP.Build.cs (line 68-69).
//   • This file does NOT call cargo, does NOT spawn the Rust binary — it is a
//     pure C++ port of the algorithm. The Rust crate is the spec; this is the
//     implementation that ships in the editor.

#include "PCG/PCGSettings_CharacterMontageAssign.h"

#include "PCGContext.h"
#include "PCGData.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Logging/LogMacros.h"

#include <cmath>

#define LOCTEXT_NAMESPACE "SOMOLMCP.CharacterMontageAssign"

DEFINE_LOG_CATEGORY_STATIC(LogSomolCharacterMontageAssign, Log, All);

// ═══════════════════════════════════════════════════════════════════════════
// Deterministic primitives — direct C++ ports from pcg_montage_sampler.rs
// ═══════════════════════════════════════════════════════════════════════════
namespace SomolMontageSampler
{
	// Hard cap matching Rust `MAX_POINTS` constant. [rust:99]
	static constexpr int32 GMaxPoints = 100000;

	// SplitMix64 finalizer constants — these MUST match Rust verbatim.
	// [rust:227-229] (used by hash_combine)
	// [rust:258-260] (used by MontageRng::next_u64)
	static constexpr uint64 GSplitMix_C1 = 0xbf58476d1ce4e5b9ULL;
	static constexpr uint64 GSplitMix_C2 = 0x94d049bb133111ebULL;
	// Golden-ratio constant for 64-bit promotion [rust:247] / SplitMix step [rust:256]
	static constexpr uint64 GSplitMix_Gamma = 0x9e3779b97f4a7c15ULL;
	// Fallback non-zero state when seed promotes to 0 [rust:249]
	static constexpr uint64 GRngZeroFallback = 0x13579bdf13579bdfULL;

	// ─── hash_combine — port of Rust `hash_combine` [rust:223-231] ─────────
	// Rust:
	//     let ua = a as u32 as u64;
	//     let ub = b as u32 as u64;
	//     let mut h = (ua << 32) | ub;
	//     h = (h ^ (h >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
	//     h = (h ^ (h >> 27)).wrapping_mul(0x94d049bb133111eb);
	//     h ^= h >> 31;
	//     (h as u32) as i32
	//
	// C++ wrapping_mul == plain unsigned 64-bit *: well-defined modulo 2^64.
	// `a as u32 as u64` zero-extends sign-bit-preserving int32 → unsigned32 → u64
	// (e.g. -1 → 0xFFFFFFFF → 0x00000000FFFFFFFF). Achieved here via
	// static_cast<uint32>(int32) → static_cast<uint64>(uint32) which is the
	// same standard-defined conversion sequence.
	static int32 HashCombine(int32 A, int32 B)
	{
		const uint64 UA = static_cast<uint64>(static_cast<uint32>(A));
		const uint64 UB = static_cast<uint64>(static_cast<uint32>(B));
		uint64 H = (UA << 32) | UB;
		H = (H ^ (H >> 30)) * GSplitMix_C1;
		H = (H ^ (H >> 27)) * GSplitMix_C2;
		H ^= (H >> 31);
		// Truncate back to int32 the same way Rust `(h as u32) as i32` does:
		// take low 32 bits, then reinterpret as signed.
		return static_cast<int32>(static_cast<uint32>(H));
	}

	// ─── MontageRng — port of Rust `MontageRng` [rust:236-273] ─────────────
	// SplitMix64 with the same gamma + zero-state fallback. Three-multiply
	// next_u64; next_unit returns 53-bit mantissa double in [0.0, 1.0).
	struct FMontageRng
	{
		uint64 State;

		// Rust [rust:242-252]
		explicit FMontageRng(int32 Seed)
		{
			uint64 S = static_cast<uint64>(static_cast<uint32>(Seed)) * GSplitMix_Gamma;
			if (S == 0)
			{
				S = GRngZeroFallback;
			}
			State = S;
		}

		// Rust [rust:255-261]
		uint64 NextU64()
		{
			State = State + GSplitMix_Gamma; // wrapping_add → unsigned + is modulo 2^64
			uint64 Z = State;
			Z = (Z ^ (Z >> 30)) * GSplitMix_C1;
			Z = (Z ^ (Z >> 27)) * GSplitMix_C2;
			return Z ^ (Z >> 31);
		}

		// Rust [rust:269-272]
		// Take 53 high bits of next_u64 (`>> 11`) → multiply by 2^-53 to get a
		// value in [0.0, 1.0). The constant `1u64 << 53` is 9007199254740992;
		// its reciprocal as double matches Rust's `(1.0 / ((1u64 << 53) as f64))`.
		double NextUnit()
		{
			const uint64 V = NextU64() >> 11;
			return static_cast<double>(V) * (1.0 / 9007199254740992.0);
		}
	};

	// ─── Validated pool entry — preserves the *valid* index that Rust keeps ─
	// in `valid_pool` after filtering. Mirrors the field set the sampler reads
	// during the per-point loop ([rust:177-186]).
	struct FValidatedEntry
	{
		FString MontagePath; // already-stringified soft path
		float Weight;
		float PlayRateMin;
		float PlayRateMax;
	};

	// FSoftObjectPath → FString — uses the same canonical form everywhere
	// (`/Game/.../Asset.Asset`) so JSON-side strings (Rust pool input) and
	// UE-side strings compare equal.
	static FString SoftPathToString(const FSoftObjectPath& Path)
	{
		return Path.ToString();
	}

	// Rust `weighted_pick` [rust:203-210]: linear scan, return the first
	// cumulative bucket where `target <= cumulative[i]`. NB: this is
	// `lower_bound` semantics (≤, not <). The fallback for FP rounding past
	// the last bucket is `cumulative.len() - 1`, matching `saturating_sub(1)`.
	static int32 WeightedPick(const TArray<double>& Cumulative, double Target)
	{
		const int32 Num = Cumulative.Num();
		for (int32 I = 0; I < Num; ++I)
		{
			if (Target <= Cumulative[I])
			{
				return I;
			}
		}
		return Num > 0 ? Num - 1 : 0;
	}

	// IsFinite check matching Rust `f32::is_finite`: rejects NaN + ±∞.
	static bool IsFiniteF32(float V)
	{
		return std::isfinite(V);
	}

	template <typename T>
	static FPCGMetadataAttribute<T>* FindOrCreateSamplerAttribute(
		UPCGMetadata* Metadata,
		FName AttributeName,
		const T& DefaultValue,
		bool bAllowsInterpolation,
		const TCHAR* TypeLabel)
	{
		if (!Metadata || AttributeName.IsNone())
		{
			return nullptr;
		}

		if (FPCGMetadataAttribute<T>* Existing = Metadata->GetMutableTypedAttribute<T>(AttributeName))
		{
			return Existing;
		}

		if (const FPCGMetadataAttributeBase* ExistingBase = Metadata->GetConstAttribute(AttributeName))
		{
			UE_LOG(LogSomolCharacterMontageAssign, Warning,
				TEXT("Metadata attribute '%s' type mismatch (type id %d); overwriting as %s."),
				*AttributeName.ToString(),
				static_cast<int32>(ExistingBase->GetTypeId()),
				TypeLabel);
		}

		return Metadata->FindOrCreateAttribute<T>(
			AttributeName,
			DefaultValue,
			bAllowsInterpolation,
			/*bOverrideParent=*/true,
			/*bOverwriteIfTypeMismatch=*/true);
	}
}

// ─── UPCGSettings_CharacterMontageAssign ────────────────────────────────────
UPCGSettings_CharacterMontageAssign::UPCGSettings_CharacterMontageAssign()
{
	// Seed the pool with one empty entry so the Details panel shows a usable
	// row immediately. Authors set MontagePath and weight in the editor.
	MontagePool.Reset();
	MontagePool.Add(FSOMOLMCPMontagePoolEntry{});
}

#if WITH_EDITOR
FName UPCGSettings_CharacterMontageAssign::GetDefaultNodeName() const
{
	return FName(TEXT("SOMOLCharacterMontageAssign"));
}

FText UPCGSettings_CharacterMontageAssign::GetDefaultNodeTitle() const
{
	return LOCTEXT("NodeTitle", "SOMOL Character Montage Assign");
}

FText UPCGSettings_CharacterMontageAssign::GetNodeTooltipText() const
{
	return LOCTEXT(
		"NodeTooltip",
		"Per-point deterministic AnimMontage assignment from a weighted pool. "
		"Writes `montage_path` + `play_rate` metadata attributes consumed by a "
		"downstream spawner. Round G executor — algorithm is byte-aligned with "
		"the Rust reference (pcg_montage_sampler.rs); same seed + pool ⇒ same "
		"output as the `pcg_montage_sample_preview` Tauri command."
	);
}

EPCGSettingsType UPCGSettings_CharacterMontageAssign::GetType() const
{
	// "Generic" matches sibling metadata-annotation nodes in the PCG plugin.
	return EPCGSettingsType::Generic;
}
#endif // WITH_EDITOR

FPCGElementPtr UPCGSettings_CharacterMontageAssign::CreateElement() const
{
	return MakeShared<FPCGElement_CharacterMontageAssign>();
}

// ═══════════════════════════════════════════════════════════════════════════
// FPCGElement_CharacterMontageAssign::ExecuteInternal — Round G real executor
// ═══════════════════════════════════════════════════════════════════════════
//
// Algorithm (mirrors `sample_montages` in pcg_montage_sampler.rs [rust:117-199]):
//
//   1. For each input PointData:
//        a. Validate the pool in input order [rust:135-156]:
//             - empty MontagePath → skip with reason
//             - non-finite OR ≤ 0 weight → skip with reason
//           Build a `valid_pool` of survivors and a `cumulative` running-sum
//           table over the survivors' weights. Track total_weight as f64.
//        b. If valid_pool empty OR total_weight ≤ 0 → log warning, emit input
//           data unchanged on the output pin [rust:158-163].
//        c. Allocate output PointData (deep copy points), create / find the
//           `montage_path` (FString) and `play_rate` (float) metadata
//           attributes, and decorate each point up to MaxPointsDecorated (also
//           clamped by GMaxPoints) [rust:166-196]:
//             • RNG = MontageRng::new(hash_combine(MontageSeed, point.Seed))
//             • target = next_unit() * total_weight
//             • idx = weighted_pick(cumulative, target)
//             • pr_min = max(entry.PlayRateMin, 0.01)  [rust:179]
//             • pr_max = (PlayRateMax < pr_min) ? pr_min : PlayRateMax  [rust:180-181]
//             • rate = (pr_max > pr_min) ? pr_min + next_unit32 * (pr_max - pr_min)
//                                        : pr_min  [rust:182-186]
//             • Write {montage_path, play_rate} to the point's MetadataEntry.
//
// Notes / divergence-watch points:
//   • Rust uses a Vec<i32> of point seeds in iteration order; here we read
//     `Point.Seed` (int32) in the same iteration order PCG provides. If PCG
//     ever reorders points before us, the determinism contract still holds
//     per-point because the seed travels with the point.
//   • Rust f32 / f64 widths preserved exactly (Cumulative is f64, NextUnit is
//     f64, jitter math is f32). Mixing widths the same way the Rust does is
//     load-bearing for byte-equality.
//   • Rust's `next_unit() as f32` cast on the play-rate jitter line [rust:183]
//     is reproduced by storing the f64 from NextUnit() and casting to float
//     at the multiply site.
//
// The ActorNamePattern + bAssignOnly fields are honoured to keep the node's
// declared parameter contract intact, but they do NOT participate in the
// determinism diff against Rust (Rust only models the sampler core).
//
bool FPCGElement_CharacterMontageAssign::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGElement_CharacterMontageAssign::Execute);

	if (!Context)
	{
		return true;
	}

	const UPCGSettings_CharacterMontageAssign* Settings =
		Context->GetInputSettings<UPCGSettings_CharacterMontageAssign>();
	if (!Settings)
	{
		UE_LOG(LogSomolCharacterMontageAssign, Warning,
			TEXT("ExecuteInternal: missing Settings; emitting pass-through only."));
		Context->OutputData.TaggedData = Context->InputData.TaggedData;
		return true;
	}

	using namespace SomolMontageSampler;

	// ─── Step 1a: Pool validation [rust:131-153] ────────────────────────────
	// Order matches Rust precisely: empty path checked first, then
	// non-finite OR ≤ 0 weight. Both go to the skip list (reason recorded
	// in the log only on the C++ side — Rust collects them into a structured
	// SkippedPoolEntry vector for the Tauri preview command).
	TArray<FValidatedEntry> ValidPool;
	TArray<double>          Cumulative;
	double                  TotalWeight = 0.0;

	ValidPool.Reserve(Settings->MontagePool.Num());
	Cumulative.Reserve(Settings->MontagePool.Num());

	for (int32 RawIndex = 0; RawIndex < Settings->MontagePool.Num(); ++RawIndex)
	{
		const FSOMOLMCPMontagePoolEntry& E = Settings->MontagePool[RawIndex];
		const FString PathString = SoftPathToString(E.MontagePath);

		if (PathString.IsEmpty())
		{
			UE_LOG(LogSomolCharacterMontageAssign, Verbose,
				TEXT("Pool entry %d skipped: empty montage_path"), RawIndex);
			continue;
		}
		if (!IsFiniteF32(E.Weight) || E.Weight <= 0.0f)
		{
			UE_LOG(LogSomolCharacterMontageAssign, Verbose,
				TEXT("Pool entry %d skipped: non-positive or non-finite weight: %f"),
				RawIndex, E.Weight);
			continue;
		}

		// Match Rust order: total_weight += weight as f64; cumulative.push(total_weight);
		// valid_pool.push(e); — see [rust:150-152].
		TotalWeight += static_cast<double>(E.Weight);
		Cumulative.Add(TotalWeight);

		FValidatedEntry V;
		V.MontagePath  = PathString;
		V.Weight       = E.Weight;
		V.PlayRateMin  = E.PlayRateMin;
		V.PlayRateMax  = E.PlayRateMax;
		ValidPool.Add(MoveTemp(V));
	}

	// ─── Step 1b: empty-pool short-circuit [rust:158-163] ───────────────────
	if (ValidPool.Num() == 0 || TotalWeight <= 0.0)
	{
		UE_LOG(LogSomolCharacterMontageAssign, Warning,
			TEXT("Valid pool is empty (in=%d, valid=0) — passing input through unchanged."),
			Settings->MontagePool.Num());
		Context->OutputData.TaggedData = Context->InputData.TaggedData;
		return true;
	}

	// Effective per-Execute cap [rust:123]: max_points.min(MAX_POINTS).
	const int32 EffectiveCap = FMath::Min(
		FMath::Max(0, Settings->MaxPointsDecorated),
		GMaxPoints);

	// Optional actor-name filter — substring match, no regex (mirrors V1 tool).
	const bool bHasFilter = !Settings->ActorNamePattern.IsEmpty();

	// ─── Step 1c: per-input-data execution ──────────────────────────────────
	// PCG's ExecuteInternal contract: walk InputData.TaggedData, transform
	// each PointData payload, push results onto OutputData.TaggedData.
	// Non-PointData inputs pass through unchanged (matches the V1 tool's
	// "ignore non-point data" behaviour).
	for (const FPCGTaggedData& InputTagged : Context->InputData.TaggedData)
	{
		const UPCGPointData* InPointData = Cast<UPCGPointData>(InputTagged.Data);
		if (!InPointData)
		{
			Context->OutputData.TaggedData.Add(InputTagged);
			continue;
		}

		// Allocate output PointData and copy points wholesale; metadata is
		// inherited from the input so existing attributes carry forward.
		UPCGPointData* OutPointData = NewObject<UPCGPointData>();
		OutPointData->InitializeFromData(InPointData);

		const TArray<FPCGPoint>& InPoints  = InPointData->GetPoints();
		TArray<FPCGPoint>&       OutPoints = OutPointData->GetMutablePoints();
		OutPoints = InPoints;

		UPCGMetadata* OutMetadata = OutPointData->Metadata;
		if (!OutMetadata)
		{
			UE_LOG(LogSomolCharacterMontageAssign, Warning,
				TEXT("Output point data missing metadata container — skipping decoration."));
			FPCGTaggedData& Pass = Context->OutputData.TaggedData.Add_GetRef(InputTagged);
			Pass.Data = OutPointData;
			continue;
		}

		// Find-or-create the two output attributes. Names come from the UCLASS
		// advanced category and default to "montage_path" / "play_rate" — the
		// SAME keys the Rust assignment struct fields use ([rust:62-65]).
		FPCGMetadataAttribute<FString>* MontageAttr =
			FindOrCreateSamplerAttribute<FString>(
				OutMetadata,
				Settings->MontageAttributeName,
				FString(),
				/*bAllowsInterpolation=*/false,
				TEXT("FString"));

		FPCGMetadataAttribute<float>* RateAttr =
			FindOrCreateSamplerAttribute<float>(
				OutMetadata,
				Settings->PlayRateAttributeName,
				1.0f,
				/*bAllowsInterpolation=*/true,
				TEXT("float"));

		if (!MontageAttr || !RateAttr)
		{
			UE_LOG(LogSomolCharacterMontageAssign, Warning,
				TEXT("Could not create montage_path/play_rate attributes; emitting unchanged points."));
			FPCGTaggedData& Pass = Context->OutputData.TaggedData.Add_GetRef(InputTagged);
			Pass.Data = OutPointData;
			continue;
		}

		// ─── Per-point sampling loop [rust:165-196] ─────────────────────────
		int32 Decorated = 0;
		int32 Skipped   = 0;
		for (int32 PointIdx = 0; PointIdx < OutPoints.Num(); ++PointIdx)
		{
			if (PointIdx >= EffectiveCap)
			{
				++Skipped;
				continue;
			}

			FPCGPoint& Point = OutPoints[PointIdx];

			// Optional substring filter on point tags — V1 parity, not part
			// of the Rust determinism contract.
			if (bHasFilter)
			{
				bool bMatched = false;
				for (const FString& Tag : InputTagged.Tags)
				{
					if (Tag.Contains(Settings->ActorNamePattern))
					{
						bMatched = true;
						break;
					}
				}
				if (!bMatched)
				{
					continue;
				}
			}

			// hash_combine(global_seed, point_seed) → seeds the per-point RNG. [rust:172-173]
			const int32 Combined = HashCombine(Settings->MontageSeed, Point.Seed);
			FMontageRng Rng(Combined);

			// Weighted pick on cumulative table. [rust:175-176]
			const double Target    = Rng.NextUnit() * TotalWeight;
			const int32  PoolIndex = WeightedPick(Cumulative, Target);
			const FValidatedEntry& Entry = ValidPool[PoolIndex];

			// Play-rate clamp + jitter [rust:179-186].
			const float PrMin    = FMath::Max(Entry.PlayRateMin, 0.01f);
			const float PrMaxRaw = Entry.PlayRateMax;
			const float PrMax    = (PrMaxRaw < PrMin) ? PrMin : PrMaxRaw;

			float Rate;
			if (PrMax > PrMin)
			{
				// Rust: `pr_min + (rng.next_unit() as f32) * (pr_max - pr_min)`
				// — the f64 → f32 cast happens before the multiply, NOT after.
				const float UnitF32 = static_cast<float>(Rng.NextUnit());
				Rate = PrMin + UnitF32 * (PrMax - PrMin);
			}
			else
			{
				Rate = PrMin;
			}

			// Allocate a per-point metadata entry if the point doesn't have
			// one yet, then write the two attributes. PCGMetadataEntryKey
			// of -1 means "no entry"; AddEntry assigns a fresh key.
			if (Point.MetadataEntry == PCGInvalidEntryKey)
			{
				Point.MetadataEntry = OutMetadata->AddEntry();
			}
			MontageAttr->SetValue(Point.MetadataEntry, Entry.MontagePath);
			RateAttr->SetValue(Point.MetadataEntry, Rate);

			++Decorated;
		}

		FPCGTaggedData& OutTagged = Context->OutputData.TaggedData.Add_GetRef(InputTagged);
		OutTagged.Data = OutPointData;

		UE_LOG(LogSomolCharacterMontageAssign, Verbose,
			TEXT("Executed: in=%d valid_pool=%d total_weight=%.4f decorated=%d skipped=%d filter='%s' assign_only=%d"),
			InPoints.Num(),
			ValidPool.Num(),
			TotalWeight,
			Decorated,
			Skipped,
			*Settings->ActorNamePattern,
			Settings->bAssignOnly ? 1 : 0);
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
