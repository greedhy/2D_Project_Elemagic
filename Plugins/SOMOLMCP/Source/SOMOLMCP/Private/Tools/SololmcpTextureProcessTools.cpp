// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — Texture processing tools (P1-6)
//
// Tools registered (all texture_* prefix):
//   texture_set_compression  — Map a string compression preset to TextureCompressionSettings::TC_*
//                              and re-encode the texture.
//   texture_generate_mips    — Set TextureMipGenSettings (TMGS_*) and trigger PostEditChange.
//   texture_channel_pack     — Read up to 4 source textures and pack a chosen channel each into a
//                              new RGBA UTexture2D at the given output_path.
//   texture_set_srgb         — Toggle UTexture::SRGB and rebuild GPU resource.
//   texture_inspect          — Return format/compression/mip/srgb/dim/disk-size metadata.
//
// Mutation pattern (per repo convention):
//   Texture->Modify() (under FScopedTransaction)
//   <mutate fields>
//   Texture->PostEditChange()       — triggers async re-encode + UpdateResource()
//   AssetMaybe->MarkPackageDirty()  (handled by SololmcpWriteFlush::EnsureFlushed)
//   SololmcpWriteFlush::EnsureFlushed(Texture)

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "PixelFormat.h"
#include "TextureResource.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::SOMOLMCP
{

// ============================================================================
// Helpers
// ============================================================================

namespace TextureProcessToolsImpl
{
	/** Resolve an asset path to a UTexture* (Texture2D or any UTexture descendant). */
	static UTexture* LoadTextureAsset(const FSololmcpToolExecutionContext& Context, const FString& AssetPath, FString& OutError)
	{
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing asset_path.");
			return nullptr;
		}
		UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		if (UTexture* AsTex = Cast<UTexture>(Asset))
		{
			return AsTex;
		}
		OutError = FString::Printf(TEXT("Asset '%s' is not a UTexture."), *AssetPath);
		return nullptr;
	}

	/** Try cast to UTexture2D; sets error if not a 2D texture. */
	static UTexture2D* AsTexture2D(UTexture* Texture, FString& OutError)
	{
		if (!Texture)
		{
			OutError = TEXT("Texture is null.");
			return nullptr;
		}
		UTexture2D* Tex2D = Cast<UTexture2D>(Texture);
		if (!Tex2D)
		{
			OutError = FString::Printf(TEXT("Texture '%s' is not a UTexture2D."), *Texture->GetPathName());
			return nullptr;
		}
		return Tex2D;
	}

	/** Map a string preset to TextureCompressionSettings. Returns false if unknown. */
	static bool ParseCompressionPreset(const FString& Preset, TextureCompressionSettings& OutValue)
	{
		const FString P = Preset.ToLower();
		if (P == TEXT("default"))                  { OutValue = TC_Default;                 return true; }
		if (P == TEXT("normalmap"))                { OutValue = TC_Normalmap;               return true; }
		if (P == TEXT("masks"))                    { OutValue = TC_Masks;                   return true; }
		if (P == TEXT("grayscale"))                { OutValue = TC_Grayscale;               return true; }
		if (P == TEXT("hdr"))                      { OutValue = TC_HDR;                     return true; }
		if (P == TEXT("alpha"))                    { OutValue = TC_Alpha;                   return true; }
		if (P == TEXT("vector_displacementmap"))   { OutValue = TC_VectorDisplacementmap;   return true; }
		if (P == TEXT("hdr_compressed"))           { OutValue = TC_HDR_Compressed;          return true; }
		if (P == TEXT("bc7"))                      { OutValue = TC_BC7;                     return true; }
		if (P == TEXT("bc6h"))                     { OutValue = TC_HDR_Compressed;          return true; } // BC6H == HDR compressed
		if (P == TEXT("uncompressed"))             { OutValue = TC_EditorIcon;              return true; } // closest mapping for "uncompressed/RGBA8"
		return false;
	}

	/** Reverse map for inspect output. */
	static FString CompressionToString(TextureCompressionSettings V)
	{
		switch (V)
		{
			case TC_Default:                return TEXT("default");
			case TC_Normalmap:              return TEXT("normalmap");
			case TC_Masks:                  return TEXT("masks");
			case TC_Grayscale:              return TEXT("grayscale");
			case TC_HDR:                    return TEXT("hdr");
			case TC_Alpha:                  return TEXT("alpha");
			case TC_VectorDisplacementmap:  return TEXT("vector_displacementmap");
			case TC_HDR_Compressed:         return TEXT("hdr_compressed");
			case TC_BC7:                    return TEXT("bc7");
			case TC_EditorIcon:             return TEXT("uncompressed");
			default:                        return UEnum::GetValueAsString(V);
		}
	}

	/** Map a string preset to TextureMipGenSettings. */
	static bool ParseMipGenPreset(const FString& Preset, TextureMipGenSettings& OutValue)
	{
		const FString P = Preset.ToLower();
		if (P == TEXT("default"))                { OutValue = TMGS_FromTextureGroup;    return true; }
		if (P == TEXT("sharpen0"))               { OutValue = TMGS_Sharpen0;            return true; }
		if (P == TEXT("sharpen5"))               { OutValue = TMGS_Sharpen5;            return true; }
		if (P == TEXT("sharpen10"))              { OutValue = TMGS_Sharpen10;           return true; }
		// UE 5.7: TMGS_AlphaDistanceField removed; map both legacy & new "angular" to TMGS_Angular
		if (P == TEXT("alpha_distance_field"))   { OutValue = TMGS_Angular;             return true; }
		if (P == TEXT("angular"))                { OutValue = TMGS_Angular;             return true; }
		if (P == TEXT("box"))                    { OutValue = TMGS_SimpleAverage;       return true; }
		if (P == TEXT("none"))                   { OutValue = TMGS_NoMipmaps;           return true; }
		return false;
	}

	static FString MipGenToString(TextureMipGenSettings V)
	{
		switch (V)
		{
			case TMGS_FromTextureGroup:   return TEXT("default");
			case TMGS_Sharpen0:           return TEXT("sharpen0");
			case TMGS_Sharpen5:           return TEXT("sharpen5");
			case TMGS_Sharpen10:          return TEXT("sharpen10");
			case TMGS_Angular:            return TEXT("angular"); // UE 5.7 replacement for legacy TMGS_AlphaDistanceField
			case TMGS_SimpleAverage:      return TEXT("box");
			case TMGS_NoMipmaps:          return TEXT("none");
			default:                      return UEnum::GetValueAsString(V);
		}
	}

	/** Returns the channel offset in a BGRA-ordered FColor (B=0, G=1, R=2, A=3). */
	static bool ParseChannelLetter(const FString& Letter, int32& OutChannelIdxBGRA)
	{
		if (Letter.IsEmpty()) { return false; }
		const TCHAR C = FChar::ToLower(Letter[0]);
		switch (C)
		{
			case TEXT('b'): OutChannelIdxBGRA = 0; return true;
			case TEXT('g'): OutChannelIdxBGRA = 1; return true;
			case TEXT('r'): OutChannelIdxBGRA = 2; return true;
			case TEXT('a'): OutChannelIdxBGRA = 3; return true;
			default:        return false;
		}
	}
}

// ============================================================================
// Tool: texture_set_compression
// ============================================================================

static void RegisterTextureSetCompression(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("texture_set_compression"),
		TEXT("Set TextureCompressionSettings via a string preset (default/normalmap/masks/grayscale/hdr/alpha/vector_displacementmap/hdr_compressed/bc7/bc6h/uncompressed) and trigger re-encoding."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Texture asset path, e.g. /Game/Textures/T_Crate"))},
			{TEXT("compression"), FSololmcpSchemaBuilder::String(TEXT("Compression preset key"),
				{TEXT("default"), TEXT("normalmap"), TEXT("masks"), TEXT("grayscale"),
				 TEXT("hdr"), TEXT("alpha"), TEXT("vector_displacementmap"),
				 TEXT("hdr_compressed"), TEXT("bc7"), TEXT("bc6h"), TEXT("uncompressed")})}
		}, {TEXT("asset_path"), TEXT("compression")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			FString Compression;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("compression"), Compression) || Compression.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("compression"));
				OutError = TEXT("Missing compression.");
				return false;
			}

			UTexture* Texture = TextureProcessToolsImpl::LoadTextureAsset(Context, AssetPath, OutError);
			if (!Texture)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}

			TextureCompressionSettings NewValue;
			if (!TextureProcessToolsImpl::ParseCompressionPreset(Compression, NewValue))
			{
				SololmcpError::InvalidType(OutStructured, TEXT("compression"), TEXT("one of the documented compression preset keys"));
				OutError = FString::Printf(TEXT("Unknown compression preset '%s'."), *Compression);
				return false;
			}

			const TextureCompressionSettings OldValue = Texture->CompressionSettings;
			const bool bChanged = (OldValue != NewValue);
			if (!bChanged)
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("compression"),
					TEXT("Texture already has the requested compression setting."));
				OutError = TEXT("Texture compression was already set to the requested value.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "TextureSetCompression", "SOMOLMCP Set Texture Compression"));
			Texture->Modify();
			Texture->CompressionSettings = NewValue;
			Texture->PostEditChange();
			SololmcpWriteFlush::EnsureFlushed(Texture);
			if (Texture->CompressionSettings != NewValue)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("compression"),
					TEXT("Texture compression readback did not match the requested value."));
				OutError = TEXT("Texture compression readback verification failed.");
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), Texture->GetPathName());
			OutStructured->SetStringField(TEXT("compression"), TextureProcessToolsImpl::CompressionToString(NewValue));
			OutStructured->SetStringField(TEXT("previous_compression"), TextureProcessToolsImpl::CompressionToString(OldValue));
			OutStructured->SetBoolField(TEXT("was_re_encoded"), bChanged);
			OutSummary = FString::Printf(TEXT("Texture '%s' compression -> %s%s"),
				*Texture->GetPathName(),
				*TextureProcessToolsImpl::CompressionToString(NewValue),
				bChanged ? TEXT(" (re-encoded)") : TEXT(" (unchanged)"));
			return true;
		}
	});
}

// ============================================================================
// Tool: texture_generate_mips
// ============================================================================

static void RegisterTextureGenerateMips(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("texture_generate_mips"),
		TEXT("Set TextureMipGenSettings (default/sharpen0/sharpen5/sharpen10/alpha_distance_field/box/none) and re-build mip chain."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Texture asset path"))},
			{TEXT("mip_gen"),    FSololmcpSchemaBuilder::String(TEXT("Mip-gen preset (default if omitted)"),
				{TEXT("default"), TEXT("sharpen0"), TEXT("sharpen5"), TEXT("sharpen10"),
				 TEXT("alpha_distance_field"), TEXT("box"), TEXT("none")})}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			UTexture* Texture = TextureProcessToolsImpl::LoadTextureAsset(Context, AssetPath, OutError);
			if (!Texture)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}

			FString MipGenStr;
			TextureMipGenSettings NewValue = TMGS_FromTextureGroup;
			if (Arguments->TryGetStringField(TEXT("mip_gen"), MipGenStr) && !MipGenStr.IsEmpty())
			{
				if (!TextureProcessToolsImpl::ParseMipGenPreset(MipGenStr, NewValue))
				{
					SololmcpError::InvalidType(OutStructured, TEXT("mip_gen"), TEXT("one of the documented mip-gen preset keys"));
					OutError = FString::Printf(TEXT("Unknown mip_gen preset '%s'."), *MipGenStr);
					return false;
				}
			}

			const TextureMipGenSettings OldValue = Texture->MipGenSettings;
			if (OldValue == NewValue)
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("mip_gen"),
					TEXT("Texture already has the requested mip generation setting."));
				OutError = TEXT("Texture mip_gen was already set to the requested value.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "TextureGenerateMips", "SOMOLMCP Set Texture MipGen"));
			Texture->Modify();
			Texture->MipGenSettings = NewValue;
			Texture->PostEditChange();
			SololmcpWriteFlush::EnsureFlushed(Texture);
			if (Texture->MipGenSettings != NewValue)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("mip_gen"),
					TEXT("Texture mip generation readback did not match the requested value."));
				OutError = TEXT("Texture mip_gen readback verification failed.");
				return false;
			}

			int32 MipCountAfter = 0;
			if (UTexture2D* Tex2D = Cast<UTexture2D>(Texture))
			{
#if WITH_EDITORONLY_DATA
				MipCountAfter = Tex2D->Source.GetNumMips();
#endif
				if (MipCountAfter == 0)
				{
					MipCountAfter = Tex2D->GetPlatformData() ? Tex2D->GetPlatformData()->Mips.Num() : 0;
				}
			}

			OutStructured->SetStringField(TEXT("asset_path"), Texture->GetPathName());
			OutStructured->SetStringField(TEXT("mip_gen"), TextureProcessToolsImpl::MipGenToString(NewValue));
			OutStructured->SetNumberField(TEXT("mip_count_after"), MipCountAfter);
			OutSummary = FString::Printf(TEXT("Texture '%s' mip_gen -> %s (mips=%d)"),
				*Texture->GetPathName(),
				*TextureProcessToolsImpl::MipGenToString(NewValue),
				MipCountAfter);
			return true;
		}
	});
}

// ============================================================================
// Tool: texture_channel_pack
// ============================================================================

static void RegisterTextureChannelPack(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("texture_channel_pack"),
		TEXT("Pack channels from up to 4 source textures into a new RGBA UTexture2D. Each source = {texture_path, channel: 'r|g|b|a'}; missing sources fill with 0 (alpha defaults 255)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Output asset path, e.g. /Game/Textures/T_Packed"))},
			{TEXT("r_source"),    FSololmcpSchemaBuilder::Object({
				{TEXT("texture_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("channel"),      FSololmcpSchemaBuilder::String(TEXT(""), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")})}
			})},
			{TEXT("g_source"),    FSololmcpSchemaBuilder::Object({
				{TEXT("texture_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("channel"),      FSololmcpSchemaBuilder::String(TEXT(""), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")})}
			})},
			{TEXT("b_source"),    FSololmcpSchemaBuilder::Object({
				{TEXT("texture_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("channel"),      FSololmcpSchemaBuilder::String(TEXT(""), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")})}
			})},
			{TEXT("a_source"),    FSololmcpSchemaBuilder::Object({
				{TEXT("texture_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("channel"),      FSololmcpSchemaBuilder::String(TEXT(""), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")})}
			})}
		}, {TEXT("output_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString OutputPath;
			if (!Arguments->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("output_path"));
				OutError = TEXT("Missing output_path.");
				return false;
			}

			// Resolve each optional source.
			struct FChannelSource
			{
				UTexture2D* Texture = nullptr;
				int32 BgraIdx = -1;     // -1 = absent
				bool bPresent = false;
			};
			FChannelSource Sources[4]; // R, G, B, A in output order
			static const TCHAR* const SourceFieldNames[4] = { TEXT("r_source"), TEXT("g_source"), TEXT("b_source"), TEXT("a_source") };

			for (int32 OutIdx = 0; OutIdx < 4; ++OutIdx)
			{
				const TSharedPtr<FJsonObject>* SrcObj = nullptr;
				if (!Arguments->TryGetObjectField(SourceFieldNames[OutIdx], SrcObj) || !SrcObj || !SrcObj->IsValid())
				{
					continue;
				}
				FString TexPath;
				FString ChanLetter;
				if (!(*SrcObj)->TryGetStringField(TEXT("texture_path"), TexPath) || TexPath.IsEmpty())
				{
					SololmcpError::Set(OutStructured, TEXT("MISSING_PARAM"), FString(SourceFieldNames[OutIdx]) + TEXT(".texture_path"),
						TEXT("Source object is present but missing texture_path."));
					OutError = FString::Printf(TEXT("Source '%s' is present but missing texture_path."), SourceFieldNames[OutIdx]);
					return false;
				}
				(*SrcObj)->TryGetStringField(TEXT("channel"), ChanLetter);

				FString LoadErr;
				UTexture* TexRaw = TextureProcessToolsImpl::LoadTextureAsset(Context, TexPath, LoadErr);
				if (!TexRaw)
				{
					SololmcpError::InvalidPath(OutStructured, TexPath);
					OutError = FString::Printf(TEXT("Source '%s' not loadable: %s"), SourceFieldNames[OutIdx], *LoadErr);
					return false;
				}
				UTexture2D* Tex2D = TextureProcessToolsImpl::AsTexture2D(TexRaw, OutError);
				if (!Tex2D)
				{
					SololmcpError::InvalidType(OutStructured, FString(SourceFieldNames[OutIdx]) + TEXT(".texture_path"), TEXT("UTexture2D"));
					return false;
				}
				int32 BgraIdx = 0;
				if (!TextureProcessToolsImpl::ParseChannelLetter(ChanLetter, BgraIdx))
				{
					SololmcpError::InvalidType(OutStructured, FString(SourceFieldNames[OutIdx]) + TEXT(".channel"), TEXT("'r' | 'g' | 'b' | 'a'"));
					OutError = FString::Printf(TEXT("Bad channel letter '%s' on %s."), *ChanLetter, SourceFieldNames[OutIdx]);
					return false;
				}
				Sources[OutIdx].Texture  = Tex2D;
				Sources[OutIdx].BgraIdx  = BgraIdx;
				Sources[OutIdx].bPresent = true;
			}

			// Determine target dimensions = max(present sources). Require at least one source.
			int32 TargetW = 0;
			int32 TargetH = 0;
			for (const FChannelSource& S : Sources)
			{
				if (!S.bPresent || !S.Texture) continue;
#if WITH_EDITORONLY_DATA
				const int32 W = S.Texture->Source.GetSizeX();
				const int32 H = S.Texture->Source.GetSizeY();
#else
				const int32 W = S.Texture->GetSizeX();
				const int32 H = S.Texture->GetSizeY();
#endif
				if (W > TargetW) TargetW = W;
				if (H > TargetH) TargetH = H;
			}
			if (TargetW <= 0 || TargetH <= 0)
			{
				SololmcpError::Set(OutStructured, TEXT("MISSING_PARAM"), TEXT("r_source|g_source|b_source|a_source"),
					TEXT("Provide at least one source texture."));
				OutError = TEXT("No valid source texture provided.");
				return false;
			}

			// Read each source's pixel data via Source.LockMip(0). Convert to BGRA8 by copying
			// the FColor channel index. If a source's dims differ from target, we sample by
			// nearest-neighbor (no filtering — channel packing usually requires same-resolution sources).
			struct FSourcePixels
			{
				TArray<FColor> Pixels;
				int32 W = 0;
				int32 H = 0;
				bool  bValid = false;
			};
			FSourcePixels SrcPixels[4];

			for (int32 OutIdx = 0; OutIdx < 4; ++OutIdx)
			{
				if (!Sources[OutIdx].bPresent || !Sources[OutIdx].Texture) continue;
				UTexture2D* Tex = Sources[OutIdx].Texture;
#if WITH_EDITORONLY_DATA
				const int32 W = Tex->Source.GetSizeX();
				const int32 H = Tex->Source.GetSizeY();
				const ETextureSourceFormat SrcFmt = Tex->Source.GetFormat();
				// TODO(P1-6): support more source formats (16-bit, HDR). For now require 8-bit BGRA.
				// UE 5.7: TSF_RGBA8 was renamed TSF_RGBA8_DEPRECATED — sources are auto-converted to
				// TSF_BGRA8 on load, so accepting BGRA8/BGRE8 covers nearly every imported 8-bit texture.
				if (SrcFmt != TSF_BGRA8 && SrcFmt != TSF_BGRE8)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
						FString::Printf(TEXT("Source '%s' has unsupported source format %d. Re-import as 8-bit BGRA."),
							SourceFieldNames[OutIdx], static_cast<int32>(SrcFmt)));
					OutError = TEXT("Unsupported source texture format for channel pack.");
					return false;
				}
				TArray64<uint8> Mip0;
				if (!Tex->Source.GetMipData(Mip0, 0))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
						FString::Printf(TEXT("Could not read mip 0 of '%s'."), *Tex->GetPathName()));
					OutError = TEXT("Source.GetMipData failed.");
					return false;
				}
				const int64 ExpectedBytes = static_cast<int64>(W) * H * 4;
				if (Mip0.Num() < ExpectedBytes)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
						FString::Printf(TEXT("Mip 0 of '%s' has %lld bytes, expected %lld."),
							*Tex->GetPathName(), static_cast<long long>(Mip0.Num()), static_cast<long long>(ExpectedBytes)));
					OutError = TEXT("Source mip data smaller than expected.");
					return false;
				}
				SrcPixels[OutIdx].Pixels.SetNumUninitialized(W * H);
				FMemory::Memcpy(SrcPixels[OutIdx].Pixels.GetData(), Mip0.GetData(), ExpectedBytes);

				// FColor uses BGRA byte order; TSF_BGRA8 source rows already match — no swap needed.
				// TSF_BGRE8 stores HDR-encoded RGB+exponent, but we treat the raw bytes as BGRA for
				// channel-pack purposes (TODO(P1-6): proper HDR decode if BGRE8 channel packing matters).

				SrcPixels[OutIdx].W = W;
				SrcPixels[OutIdx].H = H;
				SrcPixels[OutIdx].bValid = true;
#else
				SololmcpError::Set(OutStructured, TEXT("NOT_IMPLEMENTED"), TEXT(""),
					TEXT("texture_channel_pack requires WITH_EDITORONLY_DATA (editor build)."));
				OutError = TEXT("texture_channel_pack requires editor build.");
				return false;
#endif
			}

			// Build output BGRA buffer.
			TArray<FColor> OutPixels;
			OutPixels.SetNumUninitialized(TargetW * TargetH);
			for (int32 Y = 0; Y < TargetH; ++Y)
			{
				for (int32 X = 0; X < TargetW; ++X)
				{
					uint8 Channels[4] = { 0, 0, 0, 255 }; // R, G, B, A defaults
					for (int32 OutIdx = 0; OutIdx < 4; ++OutIdx)
					{
						if (!Sources[OutIdx].bPresent || !SrcPixels[OutIdx].bValid) continue;
						const int32 SX = (Sources[OutIdx].Texture && SrcPixels[OutIdx].W > 0)
							? FMath::Min<int32>(SrcPixels[OutIdx].W - 1, X * SrcPixels[OutIdx].W / TargetW)
							: 0;
						const int32 SY = (Sources[OutIdx].Texture && SrcPixels[OutIdx].H > 0)
							? FMath::Min<int32>(SrcPixels[OutIdx].H - 1, Y * SrcPixels[OutIdx].H / TargetH)
							: 0;
						const FColor& Sample = SrcPixels[OutIdx].Pixels[SY * SrcPixels[OutIdx].W + SX];
						const uint8 Component = (&Sample.B)[Sources[OutIdx].BgraIdx]; // B,G,R,A
						Channels[OutIdx] = Component;
					}
					FColor& Dest = OutPixels[Y * TargetW + X];
					Dest.R = Channels[0];
					Dest.G = Channels[1];
					Dest.B = Channels[2];
					Dest.A = Channels[3];
				}
			}

			// Create a new UTexture2D asset at OutputPath.
			FString PackagePath = OutputPath;
			int32 DotIdx = INDEX_NONE;
			if (PackagePath.FindChar('.', DotIdx))
			{
				PackagePath = PackagePath.Left(DotIdx);
			}
			if (!FPackageName::IsValidLongPackageName(PackagePath))
			{
				SololmcpError::InvalidPath(OutStructured, OutputPath);
				OutError = FString::Printf(TEXT("Invalid output_path '%s'."), *OutputPath);
				return false;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
			if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
			{
				SololmcpError::Set(OutStructured, TEXT("ALREADY_EXISTS"), TEXT("output_path"),
					FString::Printf(TEXT("Output texture already exists: %s"), *PackagePath));
				OutError = FString::Printf(TEXT("Output texture already exists: %s"), *PackagePath);
				return false;
			}
			UPackage* Package = CreatePackage(*PackagePath);
			if (!Package)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("CreatePackage returned null."));
				OutError = TEXT("Failed to create package for output texture.");
				return false;
			}

			UTexture2D* NewTex = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
			if (!NewTex)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("NewObject<UTexture2D> failed."));
				OutError = TEXT("Failed to construct UTexture2D.");
				return false;
			}

#if WITH_EDITORONLY_DATA
			NewTex->Source.Init(TargetW, TargetH, /*NumSlices=*/1, /*NumMips=*/1, TSF_BGRA8, reinterpret_cast<const uint8*>(OutPixels.GetData()));
#endif
			NewTex->SRGB = false;                       // packed masks are typically linear
			NewTex->CompressionSettings = TC_Masks;     // good default for packed channels
			NewTex->MipGenSettings = TMGS_FromTextureGroup;
			NewTex->PostEditChange();
			NewTex->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(NewTex);
			if (!UEditorAssetLibrary::SaveAsset(NewTex->GetPathName(), /*bOnlyIfIsDirty=*/false))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("output_path"),
					TEXT("UEditorAssetLibrary::SaveAsset returned false for the packed texture."));
				OutError = FString::Printf(TEXT("Failed to save packed texture '%s'."), *NewTex->GetPathName());
				return false;
			}
			SololmcpWriteFlush::EnsureFlushed(NewTex);

			// Build response.
			OutStructured->SetStringField(TEXT("output_path"), NewTex->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Dims;
			Dims.Add(MakeShared<FJsonValueNumber>(TargetW));
			Dims.Add(MakeShared<FJsonValueNumber>(TargetH));
			OutStructured->SetArrayField(TEXT("dimensions"), Dims);
			TArray<TSharedPtr<FJsonValue>> ChannelFlags;
			for (const FChannelSource& S : Sources)
			{
				ChannelFlags.Add(MakeShared<FJsonValueBoolean>(S.bPresent));
			}
			OutStructured->SetArrayField(TEXT("channels_packed"), ChannelFlags);
			OutSummary = FString::Printf(TEXT("Packed %dx%d RGBA texture '%s' (R=%d G=%d B=%d A=%d)"),
				TargetW, TargetH, *NewTex->GetPathName(),
				Sources[0].bPresent ? 1 : 0,
				Sources[1].bPresent ? 1 : 0,
				Sources[2].bPresent ? 1 : 0,
				Sources[3].bPresent ? 1 : 0);
			return true;
		}
	});
}

// ============================================================================
// Tool: texture_set_srgb
// ============================================================================

static void RegisterTextureSetSrgb(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("texture_set_srgb"),
		TEXT("Toggle UTexture::SRGB; rebuilds the GPU resource so future samples honor the new gamma assumption."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("srgb"),       FSololmcpSchemaBuilder::Boolean(TEXT("true => sRGB-encoded color, false => linear"))}
		}, {TEXT("asset_path"), TEXT("srgb")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			bool bSrgb = false;
			if (!Arguments->TryGetBoolField(TEXT("srgb"), bSrgb))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("srgb"));
				OutError = TEXT("Missing srgb (boolean).");
				return false;
			}

			UTexture* Texture = TextureProcessToolsImpl::LoadTextureAsset(Context, AssetPath, OutError);
			if (!Texture)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}

			const bool bWasDifferent = (Texture->SRGB != bSrgb);
			if (!bWasDifferent)
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("srgb"),
					TEXT("Texture already has the requested sRGB setting."));
				OutError = TEXT("Texture sRGB was already set to the requested value.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "TextureSetSrgb", "SOMOLMCP Set Texture sRGB"));
			Texture->Modify();
			Texture->SRGB = bSrgb;
			Texture->PostEditChange();
			SololmcpWriteFlush::EnsureFlushed(Texture);
			if (Texture->SRGB != bSrgb)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("srgb"),
					TEXT("Texture sRGB readback did not match the requested value."));
				OutError = TEXT("Texture sRGB readback verification failed.");
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), Texture->GetPathName());
			OutStructured->SetBoolField(TEXT("srgb"), bSrgb);
			OutStructured->SetBoolField(TEXT("requires_rebuild"), bWasDifferent);
			OutSummary = FString::Printf(TEXT("Texture '%s' sRGB=%s%s"),
				*Texture->GetPathName(),
				bSrgb ? TEXT("true") : TEXT("false"),
				bWasDifferent ? TEXT(" (resource rebuild)") : TEXT(" (no change)"));
			return true;
		}
	});
}

// ============================================================================
// Tool: texture_inspect
// ============================================================================

static void RegisterTextureInspect(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("texture_inspect"),
		TEXT("Return texture metadata: format, compression, mip-gen preset, sRGB flag, mip count, dimensions, on-disk size, virtual-texture flag."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			UTexture* Texture = TextureProcessToolsImpl::LoadTextureAsset(Context, AssetPath, OutError);
			if (!Texture)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), Texture->GetPathName());
			OutStructured->SetStringField(TEXT("class"), Texture->GetClass()->GetName());
			OutStructured->SetStringField(TEXT("compression"), TextureProcessToolsImpl::CompressionToString(Texture->CompressionSettings));
			OutStructured->SetStringField(TEXT("mip_gen"), TextureProcessToolsImpl::MipGenToString(Texture->MipGenSettings));
			OutStructured->SetBoolField(TEXT("srgb"), Texture->SRGB);
			OutStructured->SetBoolField(TEXT("virtual_texture"), Texture->VirtualTextureStreaming);

			// Dimensions + format come from UTexture2D::GetPlatformData if available, else Source.
			int32 Width = 0;
			int32 Height = 0;
			int32 MipCount = 0;
			FString FormatStr = TEXT("Unknown");

			if (UTexture2D* Tex2D = Cast<UTexture2D>(Texture))
			{
				if (FTexturePlatformData* PD = Tex2D->GetPlatformData())
				{
					Width    = PD->SizeX;
					Height   = PD->SizeY;
					MipCount = PD->Mips.Num();
					FormatStr = GetPixelFormatString(PD->PixelFormat);
				}
#if WITH_EDITORONLY_DATA
				if (Width == 0)  Width  = Tex2D->Source.GetSizeX();
				if (Height == 0) Height = Tex2D->Source.GetSizeY();
				if (MipCount == 0) MipCount = Tex2D->Source.GetNumMips();
#endif
			}

			TArray<TSharedPtr<FJsonValue>> Dims;
			Dims.Add(MakeShared<FJsonValueNumber>(Width));
			Dims.Add(MakeShared<FJsonValueNumber>(Height));
			OutStructured->SetArrayField(TEXT("dimensions"), Dims);
			OutStructured->SetStringField(TEXT("format"), FormatStr);
			OutStructured->SetNumberField(TEXT("mip_count"), MipCount);

			// On-disk package size — use UPackage file size.
			int64 SizeBytes = 0;
			if (UPackage* Pkg = Texture->GetOutermost())
			{
				FString Filename;
				if (FPackageName::DoesPackageExist(Pkg->GetName(), &Filename))
				{
					SizeBytes = IFileManager::Get().FileSize(*Filename);
					if (SizeBytes < 0) SizeBytes = 0;
				}
			}
			OutStructured->SetNumberField(TEXT("size_bytes_disk"), static_cast<double>(SizeBytes));

			OutSummary = FString::Printf(TEXT("Texture '%s' %dx%d %s mips=%d srgb=%s"),
				*Texture->GetPathName(), Width, Height, *FormatStr, MipCount,
				Texture->SRGB ? TEXT("true") : TEXT("false"));
			return true;
		}
	});
}

// ============================================================================
// Tools: texture_batch_preview / texture_batch_configure
// ============================================================================

static bool CollectBatchTextures(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TArray<UTexture2D*>& OutTextures,
	FString& OutError)
{
	TSet<FString> Seen;
	const TArray<TSharedPtr<FJsonValue>>* AssetPaths = nullptr;
	if (Arguments->TryGetArrayField(TEXT("asset_paths"), AssetPaths) && AssetPaths)
	{
		for (const TSharedPtr<FJsonValue>& Value : *AssetPaths)
		{
			FString Path;
			if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty() || Seen.Contains(Path)) continue;
			FString LoadError;
			if (UTexture2D* Texture = Cast<UTexture2D>(Context.Services.LoadAsset(Path, LoadError)))
			{
				Seen.Add(Path);
				OutTextures.Add(Texture);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* SearchPaths = nullptr;
	if (Arguments->TryGetArrayField(TEXT("search_paths"), SearchPaths) && SearchPaths && SearchPaths->Num() > 0)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		for (const TSharedPtr<FJsonValue>& Value : *SearchPaths)
		{
			FString Path;
			if (Value.IsValid() && Value->TryGetString(Path) && Path.StartsWith(TEXT("/Game"))) Filter.PackagePaths.Add(FName(*Path));
		}
		TArray<FAssetData> Assets;
		FAssetRegistryModule::GetRegistry().GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			const FString ObjectPath = Asset.GetObjectPathString();
			if (Seen.Contains(ObjectPath)) continue;
			if (UTexture2D* Texture = Cast<UTexture2D>(Asset.GetAsset()))
			{
				Seen.Add(ObjectPath);
				OutTextures.Add(Texture);
			}
		}
	}

	FString NameContains;
	Arguments->TryGetStringField(TEXT("name_contains"), NameContains);
	if (!NameContains.IsEmpty())
	{
		OutTextures.RemoveAll([&](const UTexture2D* Texture)
		{
			return !Texture || !Texture->GetName().Contains(NameContains, ESearchCase::IgnoreCase);
		});
	}

	int32 MaxAssets = 500;
	if (Arguments->HasTypedField<EJson::Number>(TEXT("max_assets"))) MaxAssets = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_assets"))), 1, 5000);
	if (OutTextures.Num() > MaxAssets) OutTextures.SetNum(MaxAssets);
	if (OutTextures.IsEmpty())
	{
		OutError = TEXT("No UTexture2D assets matched asset_paths/search_paths filters.");
		return false;
	}
	return true;
}

static bool ApplyTextureBatchSettings(UTexture2D* Texture, const TSharedPtr<FJsonObject>& Settings, TArray<FString>& OutChanged, FString& OutError)
{
	if (!Texture || !Settings.IsValid())
	{
		OutError = TEXT("Texture/settings are invalid.");
		return false;
	}
	if (Settings->HasField(TEXT("max_texture_size")))
	{
		const int32 Value = static_cast<int32>(Settings->GetNumberField(TEXT("max_texture_size")));
		if (Value != 0 && (!FMath::IsPowerOfTwo(Value) || Value < 32 || Value > 16384))
		{
			OutError = TEXT("max_texture_size must be 0 or a power of two from 32 to 16384.");
			return false;
		}
		Texture->MaxTextureSize = Value;
		OutChanged.Add(TEXT("max_texture_size"));
	}
	FString Compression;
	if (Settings->TryGetStringField(TEXT("compression"), Compression))
	{
		TextureCompressionSettings Parsed;
		if (!TextureProcessToolsImpl::ParseCompressionPreset(Compression, Parsed))
		{
			OutError = FString::Printf(TEXT("Unknown compression preset '%s'."), *Compression);
			return false;
		}
		Texture->CompressionSettings = Parsed;
		OutChanged.Add(TEXT("compression"));
	}
	FString MipGen;
	if (Settings->TryGetStringField(TEXT("mip_gen"), MipGen))
	{
		TextureMipGenSettings Parsed;
		if (!TextureProcessToolsImpl::ParseMipGenPreset(MipGen, Parsed))
		{
			OutError = FString::Printf(TEXT("Unknown mip_gen preset '%s'."), *MipGen);
			return false;
		}
		Texture->MipGenSettings = Parsed;
		OutChanged.Add(TEXT("mip_gen"));
	}
	FString LodGroup;
	if (Settings->TryGetStringField(TEXT("lod_group"), LodGroup))
	{
		const int64 Parsed = StaticEnum<TextureGroup>()->GetValueByNameString(LodGroup);
		if (Parsed == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Unknown lod_group '%s'."), *LodGroup);
			return false;
		}
		Texture->LODGroup = static_cast<TextureGroup>(Parsed);
		OutChanged.Add(TEXT("lod_group"));
	}
	FString Filter;
	if (Settings->TryGetStringField(TEXT("filter"), Filter))
	{
		if (Filter.Equals(TEXT("TF_Nearest"), ESearchCase::IgnoreCase)) Texture->Filter = TF_Nearest;
		else if (Filter.Equals(TEXT("TF_Bilinear"), ESearchCase::IgnoreCase)) Texture->Filter = TF_Bilinear;
		else if (Filter.Equals(TEXT("TF_Trilinear"), ESearchCase::IgnoreCase)) Texture->Filter = TF_Trilinear;
		else { OutError = FString::Printf(TEXT("Unknown filter '%s'."), *Filter); return false; }
		OutChanged.Add(TEXT("filter"));
	}
	bool BoolValue = false;
	if (Settings->TryGetBoolField(TEXT("srgb"), BoolValue)) { Texture->SRGB = BoolValue; OutChanged.Add(TEXT("srgb")); }
	if (Settings->TryGetBoolField(TEXT("never_stream"), BoolValue)) { Texture->NeverStream = BoolValue; OutChanged.Add(TEXT("never_stream")); }
	if (Settings->TryGetBoolField(TEXT("virtual_texture_streaming"), BoolValue)) { Texture->VirtualTextureStreaming = BoolValue; OutChanged.Add(TEXT("virtual_texture_streaming")); }
	if (Settings->HasField(TEXT("lod_bias"))) { Texture->LODBias = static_cast<int32>(Settings->GetNumberField(TEXT("lod_bias"))); OutChanged.Add(TEXT("lod_bias")); }
	return true;
}

static void RegisterTextureBatchTools(FSololmcpToolRegistry& Registry)
{
	using FSB = FSololmcpSchemaBuilder;
	auto BatchSchema = []()
	{
		return FSB::Object({
			{TEXT("asset_paths"), FSB::Array(FSB::String(), TEXT("Explicit texture asset paths."))},
			{TEXT("search_paths"), FSB::Array(FSB::String(), TEXT("Recursive /Game content roots."))},
			{TEXT("name_contains"), FSB::String(TEXT("Optional case-insensitive asset-name filter."))},
			{TEXT("max_assets"), FSB::Integer(TEXT("Safety cap, default 500, maximum 5000."))},
			{TEXT("settings"), FSB::Object({
				{TEXT("max_texture_size"), FSB::Integer(TEXT("0 or power-of-two 32..16384."))},
				{TEXT("compression"), FSB::String(TEXT("default/normalmap/masks/grayscale/hdr/alpha/bc7/etc."))},
				{TEXT("mip_gen"), FSB::String(TEXT("default/sharpen0/sharpen5/sharpen10/angular/box/none."))},
				{TEXT("lod_group"), FSB::String(TEXT("TextureGroup enum name, e.g. TEXTUREGROUP_World."))},
				{TEXT("filter"), FSB::String(TEXT("TF_Nearest/TF_Bilinear/TF_Trilinear."))},
				{TEXT("srgb"), FSB::Boolean()},
				{TEXT("never_stream"), FSB::Boolean()},
				{TEXT("virtual_texture_streaming"), FSB::Boolean()},
				{TEXT("lod_bias"), FSB::Integer()}
			}, {}, TEXT("Texture settings patch."))}
		}, {TEXT("settings")});
	};

	Registry.Register({
		TEXT("texture_batch_preview"),
		TEXT("Preview the exact UTexture2D set and parameter changes before a batch mutation. Searches only Asset Registry paths, never scans the filesystem."),
		BatchSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			TArray<UTexture2D*> Textures;
			if (!CollectBatchTextures(Context, Arguments, Textures, Error)) return false;
			TArray<TSharedPtr<FJsonValue>> Assets;
			for (UTexture2D* Texture : Textures)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("asset_path"), Texture->GetPathName());
				Row->SetNumberField(TEXT("width"), Texture->GetSizeX());
				Row->SetNumberField(TEXT("height"), Texture->GetSizeY());
				Row->SetNumberField(TEXT("max_texture_size"), Texture->MaxTextureSize);
				Assets.Add(MakeShared<FJsonValueObject>(Row));
			}
			Out->SetArrayField(TEXT("assets"), Assets);
			Out->SetNumberField(TEXT("matched_count"), Assets.Num());
			Out->SetStringField(TEXT("status"), TEXT("planned"));
			Summary = FString::Printf(TEXT("Previewed texture batch for %d assets."), Assets.Num());
			return true;
		}, nullptr, 30
	});

	Registry.Register({
		TEXT("texture_batch_configure"),
		TEXT("Batch-edit UTexture2D import/runtime parameters with transaction, save/readback receipts, per-asset errors, and a safety cap. UE 5.7/5.8 compatible."),
		BatchSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			TArray<UTexture2D*> Textures;
			if (!CollectBatchTextures(Context, Arguments, Textures, Error)) return false;
			const TSharedPtr<FJsonObject>* Settings = nullptr;
			if (!Arguments->TryGetObjectField(TEXT("settings"), Settings) || !Settings || !Settings->IsValid()) { Error = TEXT("settings is required."); return false; }
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "TextureBatchConfigure", "SOMOLMCP Batch Configure Textures"));
			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Succeeded = 0;
			int32 Failed = 0;
			for (UTexture2D* Texture : Textures)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("asset_path"), Texture->GetPathName());
				Texture->Modify();
				TArray<FString> Changed;
				FString ItemError;
				if (ApplyTextureBatchSettings(Texture, *Settings, Changed, ItemError))
				{
					Texture->PostEditChange();
					Texture->MarkPackageDirty();
					SololmcpWriteFlush::EnsureFlushed(Texture);
					Row->SetStringField(TEXT("status"), TEXT("completed"));
					Row->SetNumberField(TEXT("max_texture_size"), Texture->MaxTextureSize);
					Row->SetStringField(TEXT("compression"), TextureProcessToolsImpl::CompressionToString(Texture->CompressionSettings));
					Row->SetBoolField(TEXT("srgb"), Texture->SRGB);
					++Succeeded;
				}
				else
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("error"), ItemError);
					++Failed;
				}
				Results.Add(MakeShared<FJsonValueObject>(Row));
			}
			Out->SetArrayField(TEXT("results"), Results);
			Out->SetNumberField(TEXT("matched_count"), Textures.Num());
			Out->SetNumberField(TEXT("succeeded"), Succeeded);
			Out->SetNumberField(TEXT("failed"), Failed);
			Out->SetStringField(TEXT("status"), Failed == 0 ? TEXT("completed") : TEXT("completed_with_errors"));
			Summary = FString::Printf(TEXT("Configured %d textures; %d failed."), Succeeded, Failed);
			return Succeeded > 0;
		}, nullptr, 5
	});
}

// ============================================================================
// Registration entry point
// ============================================================================

void RegisterTextureProcessTools(FSololmcpToolRegistry& Registry)
{
	RegisterTextureSetCompression(Registry);
	RegisterTextureGenerateMips(Registry);
	RegisterTextureChannelPack(Registry);
	RegisterTextureSetSrgb(Registry);
	RegisterTextureInspect(Registry);
	RegisterTextureBatchTools(Registry);
}

} // namespace UE::SOMOLMCP
