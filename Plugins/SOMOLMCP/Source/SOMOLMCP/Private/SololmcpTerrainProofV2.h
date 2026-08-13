// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpTerrainProofV2.h
// Terrain constraint proof schema v2 classification + self-verifiable proof_sig.
//
// Contract (terrain fix batch 0+1, 2026-07-21):
//   v2 proof must contain:
//     - schema_version: "2"
//     - recipe_id (non-empty string)
//     - pre_generation_constraints (non-empty object/array/string)
//     - plan_id (non-empty string, feeds the signature)
//     - proof_sig = first 16 hex chars of sha256(recipe_id + plan_id)
//   proof_sig is self-verifiable: it is recomputed from recipe_id+plan_id at
//   validation time, so no process cache is required; proofs still validate
//   after an editor restart.
//   v1 proofs (legacy field-presence only) still pass during the compatibility
//   window but carry deprecation_warning "proof schema v1 deprecated".
//   Set SOMOLMCP_TERRAIN_PROOF_V1_COMPAT=0 (Build.cs PrivateDefinitions) to
//   remove the compatibility layer and reject v1 proofs.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#ifndef SOMOLMCP_TERRAIN_PROOF_V1_COMPAT
#define SOMOLMCP_TERRAIN_PROOF_V1_COMPAT 1
#endif

namespace SomolmcpTerrainProof
{
	// ------------------------------------------------------------------
	// Minimal self-contained SHA-256 (FIPS 180-4). UE 5.7 Core exposes no
	// usable SHA-256 (FGenericPlatformMisc::GetSHA256Signature checkf's),
	// so we carry a compact implementation instead of adding a dependency.
	// ------------------------------------------------------------------
	namespace Detail
	{
		struct FSha256Ctx
		{
			uint32 State[8];
			uint64 BitLen = 0;
			uint8 Buffer[64];
			uint32 BufferLen = 0;
		};

		static FORCEINLINE uint32 RotR(uint32 X, uint32 N) { return (X >> N) | (X << (32 - N)); }

		static void Sha256Transform(FSha256Ctx& Ctx, const uint8* Block)
		{
			static const uint32 K[64] = {
				0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
				0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
				0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
				0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
				0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
				0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
				0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
				0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
			};
			uint32 W[64];
			for (int32 I = 0; I < 16; ++I)
			{
				W[I] = (uint32(Block[I * 4]) << 24) | (uint32(Block[I * 4 + 1]) << 16) |
					(uint32(Block[I * 4 + 2]) << 8) | uint32(Block[I * 4 + 3]);
			}
			for (int32 I = 16; I < 64; ++I)
			{
				const uint32 S0 = RotR(W[I - 15], 7) ^ RotR(W[I - 15], 18) ^ (W[I - 15] >> 3);
				const uint32 S1 = RotR(W[I - 2], 17) ^ RotR(W[I - 2], 19) ^ (W[I - 2] >> 10);
				W[I] = W[I - 16] + S0 + W[I - 7] + S1;
			}
			uint32 A = Ctx.State[0], B = Ctx.State[1], C = Ctx.State[2], D = Ctx.State[3];
			uint32 E = Ctx.State[4], F = Ctx.State[5], G = Ctx.State[6], H = Ctx.State[7];
			for (int32 I = 0; I < 64; ++I)
			{
				const uint32 S1 = RotR(E, 6) ^ RotR(E, 11) ^ RotR(E, 25);
				const uint32 Ch = (E & F) ^ (~E & G);
				const uint32 T1 = H + S1 + Ch + K[I] + W[I];
				const uint32 S0 = RotR(A, 2) ^ RotR(A, 13) ^ RotR(A, 22);
				const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
				const uint32 T2 = S0 + Maj;
				H = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
			}
			Ctx.State[0] += A; Ctx.State[1] += B; Ctx.State[2] += C; Ctx.State[3] += D;
			Ctx.State[4] += E; Ctx.State[5] += F; Ctx.State[6] += G; Ctx.State[7] += H;
		}

		static void Sha256Init(FSha256Ctx& Ctx)
		{
			Ctx.State[0] = 0x6a09e667; Ctx.State[1] = 0xbb67ae85; Ctx.State[2] = 0x3c6ef372; Ctx.State[3] = 0xa54ff53a;
			Ctx.State[4] = 0x510e527f; Ctx.State[5] = 0x9b05688c; Ctx.State[6] = 0x1f83d9ab; Ctx.State[7] = 0x5be0cd19;
			Ctx.BitLen = 0;
			Ctx.BufferLen = 0;
		}

		static void Sha256Update(FSha256Ctx& Ctx, const uint8* Data, SIZE_T Len)
		{
			Ctx.BitLen += uint64(Len) * 8;
			while (Len > 0)
			{
				const uint32 Take = (uint32)FMath::Min<SIZE_T>(Len, 64 - Ctx.BufferLen);
				FMemory::Memcpy(Ctx.Buffer + Ctx.BufferLen, Data, Take);
				Ctx.BufferLen += Take;
				Data += Take;
				Len -= Take;
				if (Ctx.BufferLen == 64)
				{
					Sha256Transform(Ctx, Ctx.Buffer);
					Ctx.BufferLen = 0;
				}
			}
		}

		static void Sha256Final(FSha256Ctx& Ctx, uint8 OutHash[32])
		{
			// Padding
			Ctx.Buffer[Ctx.BufferLen++] = 0x80;
			if (Ctx.BufferLen > 56)
			{
				while (Ctx.BufferLen < 64) Ctx.Buffer[Ctx.BufferLen++] = 0;
				Sha256Transform(Ctx, Ctx.Buffer);
				Ctx.BufferLen = 0;
			}
			while (Ctx.BufferLen < 56) Ctx.Buffer[Ctx.BufferLen++] = 0;
			for (int32 I = 7; I >= 0; --I)
			{
				Ctx.Buffer[Ctx.BufferLen++] = uint8((Ctx.BitLen >> (I * 8)) & 0xff);
			}
			Sha256Transform(Ctx, Ctx.Buffer);
			for (int32 I = 0; I < 8; ++I)
			{
				OutHash[I * 4] = uint8((Ctx.State[I] >> 24) & 0xff);
				OutHash[I * 4 + 1] = uint8((Ctx.State[I] >> 16) & 0xff);
				OutHash[I * 4 + 2] = uint8((Ctx.State[I] >> 8) & 0xff);
				OutHash[I * 4 + 3] = uint8(Ctx.State[I] & 0xff);
			}
		}
	}

	/** Full sha256 of a UTF-8 rendering of Input, lowercase hex. */
	static FString Sha256Hex(const FString& Input)
	{
		FTCHARToUTF8 Utf8(*Input);
		Detail::FSha256Ctx Ctx;
		Detail::Sha256Init(Ctx);
		Detail::Sha256Update(Ctx, reinterpret_cast<const uint8*>(Utf8.Get()), (SIZE_T)Utf8.Length());
		uint8 Hash[32];
		Detail::Sha256Final(Ctx, Hash);
		FString Out;
		Out.Reserve(64);
		for (int32 I = 0; I < 32; ++I)
		{
			Out += FString::Printf(TEXT("%02x"), Hash[I]);
		}
		return Out;
	}

	/** proof_sig contract: first 16 hex chars of sha256(recipe_id + plan_id). */
	static FString ComputeProofSigV2(const FString& RecipeId, const FString& PlanId)
	{
		return Sha256Hex(RecipeId + PlanId).Left(16);
	}

	enum class EClass : uint8
	{
		None,          // no proof at all
		V1Deprecated,  // legacy field-presence proof (compat window)
		V2Valid,       // schema v2, signature verified
		V2Invalid      // claims v2 but fails validation -> must be rejected
	};

	static bool JsonValueHasProofContent(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return false;
		}
		switch (Value->Type)
		{
		case EJson::String:  return !Value->AsString().TrimStartAndEnd().IsEmpty();
		case EJson::Boolean: return Value->AsBool();
		case EJson::Number:  return true;
		case EJson::Array:   return Value->AsArray().Num() > 0;
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			return Obj.IsValid() && Obj->Values.Num() > 0;
		}
		default: return false;
		}
	}

	/** Classify one proof object (the value of terrain_constraint_proof). */
	static EClass ClassifyProofObject(const TSharedRef<FJsonObject>& Proof, FString& OutFailReason)
	{
		// v2 candidate detection: explicit schema_version or a proof_sig field.
		FString SchemaVersion;
		bool bHasSchemaVersion = Proof->TryGetStringField(TEXT("schema_version"), SchemaVersion);
		if (!bHasSchemaVersion)
		{
			double NumVersion = 0.0;
			if (Proof->TryGetNumberField(TEXT("schema_version"), NumVersion))
			{
				SchemaVersion = FString::Printf(TEXT("%d"), (int32)NumVersion);
				bHasSchemaVersion = true;
			}
		}
		FString ClaimedSig;
		const bool bHasSig = Proof->TryGetStringField(TEXT("proof_sig"), ClaimedSig) && !ClaimedSig.IsEmpty();
		if (!bHasSig)
		{
			Proof->TryGetStringField(TEXT("proofSig"), ClaimedSig);
		}
		const bool bClaimsV2 = bHasSchemaVersion || bHasSig;
		if (!bClaimsV2)
		{
			return JsonValueHasProofContent(MakeShared<FJsonValueObject>(Proof)) ? EClass::V1Deprecated : EClass::None;
		}

		if (!bHasSchemaVersion || SchemaVersion != TEXT("2"))
		{
			OutFailReason = TEXT("schema_version must be \"2\"");
			return EClass::V2Invalid;
		}
		FString RecipeId;
		if (!(Proof->TryGetStringField(TEXT("recipe_id"), RecipeId) || Proof->TryGetStringField(TEXT("recipeId"), RecipeId)) || RecipeId.IsEmpty())
		{
			OutFailReason = TEXT("missing non-empty recipe_id");
			return EClass::V2Invalid;
		}
		FString PlanId;
		if (!(Proof->TryGetStringField(TEXT("plan_id"), PlanId) || Proof->TryGetStringField(TEXT("planId"), PlanId)) || PlanId.IsEmpty())
		{
			OutFailReason = TEXT("missing non-empty plan_id");
			return EClass::V2Invalid;
		}
		TSharedPtr<FJsonValue> Constraints = Proof->TryGetField(TEXT("pre_generation_constraints"));
		if (!Constraints.IsValid())
		{
			Constraints = Proof->TryGetField(TEXT("preGenerationConstraints"));
		}
		const bool bHasConstraints = JsonValueHasProofContent(Constraints);
		if (!bHasConstraints)
		{
			OutFailReason = TEXT("missing non-empty pre_generation_constraints");
			return EClass::V2Invalid;
		}
		if (ClaimedSig.IsEmpty())
		{
			OutFailReason = TEXT("missing proof_sig");
			return EClass::V2Invalid;
		}
		const FString ExpectedSig = ComputeProofSigV2(RecipeId, PlanId);
		if (!ClaimedSig.Equals(ExpectedSig, ESearchCase::CaseSensitive))
		{
			OutFailReason = FString::Printf(TEXT("proof_sig mismatch (expected %s for recipe_id+plan_id)"), *ExpectedSig);
			return EClass::V2Invalid;
		}
		return EClass::V2Valid;
	}

	/**
	 * Classify the terrain constraint proof carried by a tool-argument object.
	 * Looks for a terrain_constraint_proof object first, then falls back to the
	 * legacy loose-field proof surface (v1).
	 */
	static EClass Classify(const TSharedRef<FJsonObject>& Args, FString& OutFailReason)
	{
		OutFailReason.Reset();
		static const TCHAR* ProofKeys[] = { TEXT("terrain_constraint_proof"), TEXT("terrainConstraintProof") };
		for (const TCHAR* Key : ProofKeys)
		{
			const TSharedPtr<FJsonObject>* ProofObj = nullptr;
			if (Args->TryGetObjectField(Key, ProofObj) && ProofObj && ProofObj->IsValid())
			{
				return ClassifyProofObject(ProofObj->ToSharedRef(), OutFailReason);
			}
		}

		// Legacy v1 loose-field surface (kept in sync with
		// SololmcpLargeWorldTools.cpp HasTerrainConstraintProofForGeneration).
		static const TCHAR* LegacyKeys[] = {
			TEXT("pre_generation_constraints"), TEXT("preGenerationConstraints"),
			TEXT("constrained_heightmap_recipe"), TEXT("constrainedHeightmapRecipe"),
			TEXT("terrain_geomorphology_plan"), TEXT("terrainGeomorphologyPlan"),
			TEXT("landform_constraints"), TEXT("constraint_profile")
		};
		for (const TCHAR* Key : LegacyKeys)
		{
			const TSharedPtr<FJsonValue> Value = Args->TryGetField(Key);
			if (JsonValueHasProofContent(Value))
			{
				return EClass::V1Deprecated;
			}
		}
		return EClass::None;
	}

	static const TCHAR* DeprecationWarningText()
	{
		return TEXT("proof schema v1 deprecated");
	}
}
