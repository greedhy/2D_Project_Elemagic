// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v3.4 — Material Template Tools
// PBR, Toon, Outline, Dissolve, Hologram, Fresnel, Wind, Diagnose, Repair
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpWriteFlush.h"
#include "SololmcpErrorHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionStep.h"
#include "Materials/MaterialExpressionSmoothStep.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"

#include "MaterialEditingLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialDomain.h"
#include "Engine/Texture.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/MeshComponent.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP {
namespace {
bool SplitPath(const FString& A,FString& P,FString& N){int32 i;if(!A.FindLastChar('/',i)||i<=0)return false;P=A.Left(i);N=A.RightChop(i+1);return true;}
UMaterial* CreateMat(FSololmcpEditorServices& S,const FString& P,const FString& N,FString& E){
	FString Ef=S.GenerateUniqueAssetName(P,N);const FScopedTransaction Tr(NSLOCTEXT("SOMOLMCP","MT","MT"));
	UObject* A=S.CreateAsset(P,Ef,UMaterial::StaticClass()->GetPathName(),UMaterialFactoryNew::StaticClass()->GetPathName(),nullptr,E);return Cast<UMaterial>(A);}
template<typename T>T* AddNode(UMaterial* M,int Px,int Py){return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(M,T::StaticClass(),Px,Py));}
UMaterialExpression* AddNodeByClassName(UMaterial* M,const TCHAR* ClassPath,int Px,int Py){
	UClass* ExpressionClass=FindObject<UClass>(nullptr,ClassPath);
	if(!ExpressionClass)ExpressionClass=LoadObject<UClass>(nullptr,ClassPath);
	return ExpressionClass?UMaterialEditingLibrary::CreateMaterialExpression(M,ExpressionClass,Px,Py):nullptr;
}
bool Conn(UMaterialExpression* F,const FString& O,UMaterialExpression* T,const FString& I){return UMaterialEditingLibrary::ConnectMaterialExpressions(F,O,T,I);}
bool ConnProp(UMaterialExpression* E,const FString& O,EMaterialProperty P){return UMaterialEditingLibrary::ConnectMaterialProperty(E,O,P);}
FString GetPath(UMaterial* M){FString P=M->GetPathName();int32 D;if(P.FindChar('.',D))P=P.Left(D);return P;}
bool Finish(FSololmcpEditorServices& S,UMaterial* M,const FString& P,TSharedRef<FJsonObject>& O,FString& Su,FString& E){
	if(!M){E=TEXT("Finish: null material");return false;}
	UClass* ExpectedClass=M->GetClass();
	M->PreEditChange(nullptr);M->PostEditChange();
	if(!S.SaveAsset(P,false,E))return false;
	if(!S.AssetExists(P)){E=FString::Printf(TEXT("Material save reported success but asset does not exist: %s"),*P);return false;}
	FString ReloadErr;UObject* Reloaded=S.LoadAsset(P,ReloadErr);
	if(!Reloaded||!Reloaded->IsA(ExpectedClass)){E=FString::Printf(TEXT("Material reload/class validation failed for %s: %s"),*P,*ReloadErr);return false;}
	O->SetStringField(TEXT("asset_path"),P);O->SetNumberField(TEXT("expression_count"),M->GetExpressions().Num());O->SetStringField(TEXT("status"),TEXT("saved_verified"));return true;}
FLinearColor Hex(const FString& H){return H.IsEmpty()?FLinearColor::White:FLinearColor(FColor::FromHex(H));}
UMaterial* LoadMat(FSololmcpEditorServices& S,const FString& Path,FString& Er){
	UObject* O=S.LoadAsset(Path,Er);if(!O)return nullptr;
	UMaterial* M=Cast<UMaterial>(O);if(!M){Er=TEXT("Not a Material");return nullptr;}return M;}
UMaterialInterface* LoadMaterialInterface(FSololmcpEditorServices& S,const FString& Path,FString& Er){
	UObject* O=S.LoadAsset(Path,Er);if(!O)return nullptr;
	UMaterialInterface* M=Cast<UMaterialInterface>(O);
	if(!M){Er=TEXT("Asset is not a Material or MaterialInstance");return nullptr;}
	return M;}
}
void RegisterMaterialTemplateTools(FSololmcpToolRegistry& R)
{
	using SB=FSololmcpSchemaBuilder;

	// ==== material_create_pbr ====
	R.Register({TEXT("material_create_pbr"),
		TEXT("Create PBR material with TextureSampleParameter2D (Albedo,Normal,Roughness,Metallic,AO,Emissive). Scalar/Vector fallback for missing textures."),
		SB::Object({{TEXT("asset_path"),SB::String(TEXT("/Game/Folder/M_Name"))},{TEXT("albedo_texture"),SB::String()},{TEXT("normal_texture"),SB::String()},
			{TEXT("roughness_texture"),SB::String()},{TEXT("metallic_texture"),SB::String()},{TEXT("ao_texture"),SB::String()},{TEXT("emissive_texture"),SB::String()},
			{TEXT("roughness_value"),SB::Number(TEXT("0-1 (0.5)"))},{TEXT("metallic_value"),SB::Number(TEXT("0-1 (0.0)"))},
			{TEXT("base_color"),SB::String(TEXT("Hex"))},{TEXT("two_sided"),SB::Boolean()},{TEXT("blend_mode"),SB::String(TEXT("Opaque/Masked/Translucent"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad asset_path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;
			FString Bm;if(A->TryGetStringField(TEXT("blend_mode"),Bm)){if(Bm==TEXT("Masked"))M->BlendMode=BLEND_Masked;else if(Bm==TEXT("Translucent"))M->BlendMode=BLEND_Translucent;}
			if(A->HasTypedField<EJson::Boolean>(TEXT("two_sided"))&&A->GetBoolField(TEXT("two_sided")))M->TwoSided=1;
			FString AT,NT,RT,MT,AoT,ET;
			A->TryGetStringField(TEXT("albedo_texture"),AT);A->TryGetStringField(TEXT("normal_texture"),NT);
			A->TryGetStringField(TEXT("roughness_texture"),RT);A->TryGetStringField(TEXT("metallic_texture"),MT);
			A->TryGetStringField(TEXT("ao_texture"),AoT);A->TryGetStringField(TEXT("emissive_texture"),ET);
			auto TexP=[&](const FString& PN,const FString& TPath,EMaterialProperty Pr,int Y){
				auto* T=AddNode<UMaterialExpressionTextureSampleParameter2D>(M,-800,Y);if(!T)return;
				T->ParameterName=FName(*PN);if(!TPath.IsEmpty()){if(auto* Tx=LoadObject<UTexture>(nullptr,*TPath))T->Texture=Tx;}
				ConnProp(T,(Pr==MP_Normal)?TEXT("XY"):TEXT("RGB"),Pr);};
			TexP(TEXT("Albedo"),AT,MP_BaseColor,0);TexP(TEXT("Normal"),NT,MP_Normal,200);
			TexP(TEXT("Roughness"),RT,MP_Roughness,400);TexP(TEXT("Metallic"),MT,MP_Metallic,600);
			TexP(TEXT("AO"),AoT,MP_AmbientOcclusion,800);TexP(TEXT("Emissive"),ET,MP_EmissiveColor,1000);
			if(RT.IsEmpty()){float V=A->HasTypedField<EJson::Number>(TEXT("roughness_value"))?(float)A->GetNumberField(TEXT("roughness_value")):0.5f;
				auto* S=AddNode<UMaterialExpressionScalarParameter>(M,-400,400);if(S){S->ParameterName=FName(TEXT("Roughness"));S->DefaultValue=V;ConnProp(S,TEXT(""),MP_Roughness);}}
			if(MT.IsEmpty()){float V=A->HasTypedField<EJson::Number>(TEXT("metallic_value"))?(float)A->GetNumberField(TEXT("metallic_value")):0.f;
				auto* S=AddNode<UMaterialExpressionScalarParameter>(M,-400,600);if(S){S->ParameterName=FName(TEXT("Metallic"));S->DefaultValue=V;ConnProp(S,TEXT(""),MP_Metallic);}}
			if(AT.IsEmpty()){FString H;if(A->TryGetStringField(TEXT("base_color"),H)&&!H.IsEmpty()){
				auto* V=AddNode<UMaterialExpressionVectorParameter>(M,-400,0);if(V){V->ParameterName=FName(TEXT("BaseColor"));V->DefaultValue=Hex(H);ConnProp(V,TEXT(""),MP_BaseColor);}}}
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}
			O->SetBoolField(TEXT("has_albedo"),!AT.IsEmpty());O->SetBoolField(TEXT("has_normal"),!NT.IsEmpty());
			Su=FString::Printf(TEXT("Created PBR: %s"),*P);return true;}
	,nullptr,5});

	// ==== material_create_pbr_simple ====
	R.Register({TEXT("material_create_pbr_simple"),
		TEXT("Simple PBR: VectorParameter(BaseColor) + ScalarParameter(Roughness/Metallic). Optional emissive."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("base_color"),SB::String(TEXT("Hex"))},{TEXT("roughness"),SB::Number(TEXT("0-1"))},
			{TEXT("metallic"),SB::Number(TEXT("0-1"))},{TEXT("emissive_strength"),SB::Number(TEXT("0=off"))},{TEXT("emissive_color"),SB::String(TEXT("Hex"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;
			FString H;FLinearColor BC=FLinearColor::White;if(A->TryGetStringField(TEXT("base_color"),H)&&!H.IsEmpty())BC=Hex(H);
			auto* VP=AddNode<UMaterialExpressionVectorParameter>(M,-400,0);if(VP){VP->ParameterName=FName(TEXT("BaseColor"));VP->DefaultValue=BC;ConnProp(VP,TEXT(""),MP_BaseColor);}
			float Rv=A->HasTypedField<EJson::Number>(TEXT("roughness"))?(float)A->GetNumberField(TEXT("roughness")):0.5f;
			auto* RS=AddNode<UMaterialExpressionScalarParameter>(M,-400,200);if(RS){RS->ParameterName=FName(TEXT("Roughness"));RS->DefaultValue=Rv;ConnProp(RS,TEXT(""),MP_Roughness);}
			float Mt=A->HasTypedField<EJson::Number>(TEXT("metallic"))?(float)A->GetNumberField(TEXT("metallic")):0.f;
			auto* MS=AddNode<UMaterialExpressionScalarParameter>(M,-400,400);if(MS){MS->ParameterName=FName(TEXT("Metallic"));MS->DefaultValue=Mt;ConnProp(MS,TEXT(""),MP_Metallic);}
			float Em=A->HasTypedField<EJson::Number>(TEXT("emissive_strength"))?(float)A->GetNumberField(TEXT("emissive_strength")):0.f;
			if(Em>0.f){FString EH;FLinearColor EC=BC;if(A->TryGetStringField(TEXT("emissive_color"),EH)&&!EH.IsEmpty())EC=Hex(EH);
				auto* EVP=AddNode<UMaterialExpressionVectorParameter>(M,-800,0);
				auto* ESS=AddNode<UMaterialExpressionScalarParameter>(M,-800,200);
				auto* Mul=AddNode<UMaterialExpressionMultiply>(M,-600,0);
				if(EVP&&ESS&&Mul){EVP->ParameterName=FName(TEXT("EmissiveColor"));EVP->DefaultValue=EC;
					ESS->ParameterName=FName(TEXT("EmissiveStrength"));ESS->DefaultValue=Em;
					Conn(EVP,TEXT("RGB"),Mul,TEXT("A"));Conn(ESS,TEXT(""),Mul,TEXT("B"));ConnProp(Mul,TEXT("RGB"),MP_EmissiveColor);}}
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}Su=FString::Printf(TEXT("Created PBR simple: %s"),*P);return true;}
	,nullptr,5});
	// ==== material_create_toon ====
	R.Register({TEXT("material_create_toon"),
		TEXT("Cel-shading with Custom node (floor(NdotL*Steps)/Steps), configurable bands, shadow color, fresnel rim."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("base_color"),SB::String(TEXT("Hex '#4488FF'"))},{TEXT("shadow_color"),SB::String(TEXT("Hex '#223355'"))},
			{TEXT("steps"),SB::Number(TEXT("2-8"))},{TEXT("rim_light"),SB::Boolean(TEXT("true"))},{TEXT("rim_power"),SB::Number(TEXT("3.0"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;
			FString H;FLinearColor BC=Hex(TEXT("4488FF"));if(A->TryGetStringField(TEXT("base_color"),H)&&!H.IsEmpty())BC=Hex(H);
			FLinearColor SC=Hex(TEXT("223355"));if(A->TryGetStringField(TEXT("shadow_color"),H)&&!H.IsEmpty())SC=Hex(H);
			int32 St=FMath::Clamp(A->HasTypedField<EJson::Number>(TEXT("steps"))?(int32)A->GetNumberField(TEXT("steps")):3,2,8);
			bool bRim=!A->HasTypedField<EJson::Boolean>(TEXT("rim_light"))||A->GetBoolField(TEXT("rim_light"));
			float RP=A->HasTypedField<EJson::Number>(TEXT("rim_power"))?(float)A->GetNumberField(TEXT("rim_power")):3.f;
			auto* BCP=AddNode<UMaterialExpressionVectorParameter>(M,-1200,0);if(BCP){BCP->ParameterName=FName(TEXT("BaseColor"));BCP->DefaultValue=BC;}
			auto* SCP=AddNode<UMaterialExpressionVectorParameter>(M,-1200,300);if(SCP){SCP->ParameterName=FName(TEXT("ShadowColor"));SCP->DefaultValue=SC;}
			auto* STP=AddNode<UMaterialExpressionScalarParameter>(M,-1200,600);if(STP){STP->ParameterName=FName(TEXT("Steps"));STP->DefaultValue=(float)St;}
			auto* Fr=AddNode<UMaterialExpressionFresnel>(M,-1200,900);if(Fr)Fr->Exponent=RP;
			auto* Cu=AddNode<UMaterialExpressionCustom>(M,-600,300);
			if(Cu){Cu->Code=TEXT("float N=dot(normalize(Normal),normalize(LightDir));N=N*0.5+0.5;return floor(N*Steps)/Steps;");
				Cu->OutputType=ECustomMaterialOutputType::CMOT_Float1;
				FCustomInput CN;CN.InputName=FName(TEXT("Normal"));
				FCustomInput CL;CL.InputName=FName(TEXT("LightDir"));
				FCustomInput CS;CS.InputName=FName(TEXT("Steps"));
				Cu->Inputs.Add(CN);Cu->Inputs.Add(CL);Cu->Inputs.Add(CS);}
			auto* VN=AddNode<UMaterialExpressionVertexNormalWS>(M,-1000,200);auto* CV=AddNode<UMaterialExpressionCameraVectorWS>(M,-1000,400);
			if(VN&&Cu)Conn(VN,TEXT(""),Cu,TEXT("Normal"));if(CV&&Cu)Conn(CV,TEXT(""),Cu,TEXT("LightDir"));if(STP&&Cu)Conn(STP,TEXT(""),Cu,TEXT("Steps"));
			auto* Le=AddNode<UMaterialExpressionLinearInterpolate>(M,-300,200);
			if(Le){Conn(SCP,TEXT("RGB"),Le,TEXT("A"));Conn(BCP,TEXT("RGB"),Le,TEXT("B"));if(Cu)Conn(Cu,TEXT(""),Le,TEXT("Alpha"));ConnProp(Le,TEXT("RGB"),MP_BaseColor);}
			if(bRim&&BCP&&Fr){auto* Mu=AddNode<UMaterialExpressionMultiply>(M,-300,500);auto* Ad=AddNode<UMaterialExpressionAdd>(M,-100,300);
				if(Mu&&Ad){Conn(Fr,TEXT(""),Mu,TEXT("A"));Conn(BCP,TEXT("RGB"),Mu,TEXT("B"));Conn(Le,TEXT("RGB"),Ad,TEXT("A"));Conn(Mu,TEXT("RGB"),Ad,TEXT("B"));ConnProp(Ad,TEXT("RGB"),MP_BaseColor);}}
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}O->SetNumberField(TEXT("steps"),St);O->SetBoolField(TEXT("rim_light"),bRim);
			Su=FString::Printf(TEXT("Created toon: %s (%d steps)"),*P,St);return true;}
	,nullptr,5});

	// ==== material_create_toon_outline ====
	R.Register({TEXT("material_create_toon_outline"),
		TEXT("Inverted-hull outline: VertexNormal*Thickness -> WPO. Apply to duplicated mesh with flipped normals."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("outline_color"),SB::String(TEXT("Hex '#000000'"))},{TEXT("thickness"),SB::Number(TEXT("0.02"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;
			FString H;FLinearColor OC=FLinearColor::Black;if(A->TryGetStringField(TEXT("outline_color"),H)&&!H.IsEmpty())OC=Hex(H);
			float Th=A->HasTypedField<EJson::Number>(TEXT("thickness"))?(float)A->GetNumberField(TEXT("thickness")):0.02f;
			auto* OCP=AddNode<UMaterialExpressionVectorParameter>(M,-400,0);if(OCP){OCP->ParameterName=FName(TEXT("OutlineColor"));OCP->DefaultValue=OC;ConnProp(OCP,TEXT(""),MP_BaseColor);}
			auto* TP=AddNode<UMaterialExpressionScalarParameter>(M,-800,300);if(TP){TP->ParameterName=FName(TEXT("OutlineThickness"));TP->DefaultValue=Th;}
			auto* VN=AddNode<UMaterialExpressionVertexNormalWS>(M,-800,0);
			auto* Mu=AddNode<UMaterialExpressionMultiply>(M,-600,100);if(Mu&&VN&&TP){Conn(VN,TEXT(""),Mu,TEXT("A"));Conn(TP,TEXT(""),Mu,TEXT("B"));ConnProp(Mu,TEXT("RGB"),MP_WorldPositionOffset);}
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}O->SetNumberField(TEXT("thickness"),(double)Th);
			Su=FString::Printf(TEXT("Created outline: %s"),*P);return true;}
	,nullptr,5});

	// ==== material_create_dissolve ====
	R.Register({TEXT("material_create_dissolve"),
		TEXT("Dissolve: Noise+Step for mask, SmoothStep edge glow. Masked blend."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("base_color"),SB::String(TEXT("Hex"))},{TEXT("edge_color"),SB::String(TEXT("Hex '#FF6600'"))},
			{TEXT("edge_width"),SB::Number(TEXT("0.05"))},{TEXT("noise_scale"),SB::Number(TEXT("10.0"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;M->BlendMode=BLEND_Masked;
			FString H;FLinearColor BC=FLinearColor::White;if(A->TryGetStringField(TEXT("base_color"),H)&&!H.IsEmpty())BC=Hex(H);
			FLinearColor EC=Hex(TEXT("FF6600"));if(A->TryGetStringField(TEXT("edge_color"),H)&&!H.IsEmpty())EC=Hex(H);
			float EW=A->HasTypedField<EJson::Number>(TEXT("edge_width"))?(float)A->GetNumberField(TEXT("edge_width")):0.05f;
			float NS=A->HasTypedField<EJson::Number>(TEXT("noise_scale"))?(float)A->GetNumberField(TEXT("noise_scale")):10.f;
			auto* BCP=AddNode<UMaterialExpressionVectorParameter>(M,-1200,0);if(BCP){BCP->ParameterName=FName(TEXT("BaseColor"));BCP->DefaultValue=BC;ConnProp(BCP,TEXT(""),MP_BaseColor);}
			auto* ThP=AddNode<UMaterialExpressionScalarParameter>(M,-1200,300);if(ThP){ThP->ParameterName=FName(TEXT("DissolveThreshold"));ThP->DefaultValue=0.5f;}
			auto* Ns=AddNode<UMaterialExpressionNoise>(M,-800,0);if(Ns){Ns->Scale=NS;}
			auto* St=AddNode<UMaterialExpressionStep>(M,-600,200);if(St){if(ThP)Conn(ThP,TEXT(""),St,TEXT("Y"));if(Ns)Conn(Ns,TEXT(""),St,TEXT("X"));ConnProp(St,TEXT(""),MP_OpacityMask);}
			auto* EWP=AddNode<UMaterialExpressionScalarParameter>(M,-800,400);if(EWP){EWP->ParameterName=FName(TEXT("EdgeWidth"));EWP->DefaultValue=EW;}
			auto* ECP=AddNode<UMaterialExpressionVectorParameter>(M,-1200,600);if(ECP){ECP->ParameterName=FName(TEXT("EdgeColor"));ECP->DefaultValue=EC;}
			auto* Sb=AddNode<UMaterialExpressionSubtract>(M,-600,500);if(Sb&&ThP&&EWP){Conn(ThP,TEXT(""),Sb,TEXT("A"));Conn(EWP,TEXT(""),Sb,TEXT("B"));}
			auto* Sm=AddNode<UMaterialExpressionSmoothStep>(M,-400,400);if(Sm&&Sb&&ThP&&Ns){Conn(Sb,TEXT(""),Sm,TEXT("Low"));Conn(ThP,TEXT(""),Sm,TEXT("High"));Conn(Ns,TEXT(""),Sm,TEXT("X"));}
			auto* Mu=AddNode<UMaterialExpressionMultiply>(M,-200,400);if(Mu&&Sm&&ECP){Conn(Sm,TEXT(""),Mu,TEXT("A"));Conn(ECP,TEXT("RGB"),Mu,TEXT("B"));ConnProp(Mu,TEXT("RGB"),MP_EmissiveColor);}
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}Su=FString::Printf(TEXT("Created dissolve: %s"),*P);return true;}
	,nullptr,5});
	// ==== material_create_hologram ====
	R.Register({TEXT("material_create_hologram"),
		TEXT("Hologram: scan lines + fresnel + depth fade. Translucent+TwoSided."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("color"),SB::String(TEXT("Hex '#00FFFF'"))},
			{TEXT("scan_speed"),SB::Number(TEXT("2.0"))},{TEXT("scan_density"),SB::Number(TEXT("50.0"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;M->BlendMode=BLEND_Translucent;M->TwoSided=1;
			FString Hx;FLinearColor HC=Hex(TEXT("00FFFF"));if(A->TryGetStringField(TEXT("color"),Hx)&&!Hx.IsEmpty())HC=Hex(Hx);
			float Spd=A->HasTypedField<EJson::Number>(TEXT("scan_speed"))?(float)A->GetNumberField(TEXT("scan_speed")):2.f;
			float Den=A->HasTypedField<EJson::Number>(TEXT("scan_density"))?(float)A->GetNumberField(TEXT("scan_density")):50.f;
			auto* CP_=AddNode<UMaterialExpressionVectorParameter>(M,-1400,0);if(CP_){CP_->ParameterName=FName(TEXT("HoloColor"));CP_->DefaultValue=HC;}
			auto* WP=AddNode<UMaterialExpressionWorldPosition>(M,-1400,300);
			auto* YM=AddNode<UMaterialExpressionComponentMask>(M,-1200,300);if(YM){YM->R=0;YM->G=1;YM->B=0;YM->A=0;}if(WP&&YM)Conn(WP,TEXT(""),YM,TEXT(""));
			auto* DP=AddNode<UMaterialExpressionScalarParameter>(M,-1400,600);if(DP){DP->ParameterName=FName(TEXT("ScanDensity"));DP->DefaultValue=Den;}
			auto* M1=AddNode<UMaterialExpressionMultiply>(M,-1000,300);if(M1&&YM&&DP){Conn(YM,TEXT(""),M1,TEXT("A"));Conn(DP,TEXT(""),M1,TEXT("B"));}
			auto* Ti=AddNode<UMaterialExpressionTime>(M,-1400,900);
			auto* SP_=AddNode<UMaterialExpressionScalarParameter>(M,-1400,1100);if(SP_){SP_->ParameterName=FName(TEXT("ScanSpeed"));SP_->DefaultValue=Spd;}
			auto* M2=AddNode<UMaterialExpressionMultiply>(M,-1200,900);if(M2&&Ti&&SP_){Conn(Ti,TEXT(""),M2,TEXT("A"));Conn(SP_,TEXT(""),M2,TEXT("B"));}
			auto* A1=AddNode<UMaterialExpressionAdd>(M,-800,500);if(A1&&M1&&M2){Conn(M1,TEXT(""),A1,TEXT("A"));Conn(M2,TEXT(""),A1,TEXT("B"));}
			auto* Fc=AddNode<UMaterialExpressionFrac>(M,-600,500);if(Fc&&A1)Conn(A1,TEXT(""),Fc,TEXT(""));
			auto* M3=AddNode<UMaterialExpressionMultiply>(M,-400,500);
			{auto* C2=AddNode<UMaterialExpressionConstant>(M,-600,700);if(C2)C2->R=2.f;if(M3&&Fc&&C2){Conn(Fc,TEXT(""),M3,TEXT("A"));Conn(C2,TEXT(""),M3,TEXT("B"));}}
			auto* Sb=AddNode<UMaterialExpressionSubtract>(M,-200,500);
			{auto* C9=AddNode<UMaterialExpressionConstant>(M,-400,700);if(C9)C9->R=0.95f;if(Sb&&M3&&C9){Conn(M3,TEXT(""),Sb,TEXT("A"));Conn(C9,TEXT(""),Sb,TEXT("B"));}}
			auto* Sa=AddNode<UMaterialExpressionSaturate>(M,0,500);if(Sa&&Sb)Conn(Sb,TEXT(""),Sa,TEXT(""));
			auto* Fe=AddNode<UMaterialExpressionFresnel>(M,-800,0);
			auto* MF=AddNode<UMaterialExpressionMultiply>(M,-600,0);
			{auto* CF=AddNode<UMaterialExpressionConstant>(M,-800,200);if(CF)CF->R=0.5f;if(MF&&Fe&&CF){Conn(Fe,TEXT(""),MF,TEXT("A"));Conn(CF,TEXT(""),MF,TEXT("B"));}}
			auto* AF=AddNode<UMaterialExpressionAdd>(M,-200,200);if(AF&&Sa&&MF){Conn(Sa,TEXT(""),AF,TEXT("A"));Conn(MF,TEXT(""),AF,TEXT("B"));}
			auto* MC=AddNode<UMaterialExpressionMultiply>(M,0,0);if(MC&&CP_&&AF){Conn(CP_,TEXT("RGB"),MC,TEXT("A"));Conn(AF,TEXT(""),MC,TEXT("B"));ConnProp(MC,TEXT("RGB"),MP_EmissiveColor);}
			auto* OM=AddNode<UMaterialExpressionMultiply>(M,200,200);if(OM&&CP_&&AF){Conn(CP_,TEXT("A"),OM,TEXT("A"));Conn(AF,TEXT(""),OM,TEXT("B"));}
		#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 5
			auto* DF=AddNodeByClassName(M,TEXT("/Script/Engine.MaterialExpressionDepthFade"),200,0);
			if(DF&&OM){Conn(OM,TEXT(""),DF,TEXT("In"));if(FFloatProperty* P=FindFProperty<FFloatProperty>(DF->GetClass(),TEXT("FadeDistanceDefault")))P->SetPropertyValue_InContainer(DF,0.5f);ConnProp(DF,TEXT(""),MP_Opacity);}
		#else
			auto* DF=AddNode<UMaterialExpressionDepthFade>(M,200,0);if(DF&&OM){Conn(OM,TEXT(""),DF,TEXT("In"));DF->FadeDistanceDefault=0.5f;ConnProp(DF,TEXT(""),MP_Opacity);}
		#endif
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}Su=FString::Printf(TEXT("Created hologram: %s"),*P);return true;}
	,nullptr,5});

	// ==== material_create_fresnel_rim ====
	R.Register({TEXT("material_create_fresnel_rim"),
		TEXT("Fresnel rim-light: base + edge glow. Shields, energy, highlights."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("base_color"),SB::String(TEXT("Hex '#333333'"))},
			{TEXT("rim_color"),SB::String(TEXT("Hex '#4488FF'"))},{TEXT("rim_power"),SB::Number(TEXT("3.0"))},{TEXT("rim_intensity"),SB::Number(TEXT("2.0"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;
			FString H;FLinearColor BC=Hex(TEXT("333333"));if(A->TryGetStringField(TEXT("base_color"),H)&&!H.IsEmpty())BC=Hex(H);
			FLinearColor RC=Hex(TEXT("4488FF"));if(A->TryGetStringField(TEXT("rim_color"),H)&&!H.IsEmpty())RC=Hex(H);
			float RP=A->HasTypedField<EJson::Number>(TEXT("rim_power"))?(float)A->GetNumberField(TEXT("rim_power")):3.f;
			float RI=A->HasTypedField<EJson::Number>(TEXT("rim_intensity"))?(float)A->GetNumberField(TEXT("rim_intensity")):2.f;
			auto* BCP=AddNode<UMaterialExpressionVectorParameter>(M,-1000,0);if(BCP){BCP->ParameterName=FName(TEXT("BaseColor"));BCP->DefaultValue=BC;ConnProp(BCP,TEXT(""),MP_BaseColor);}
			auto* RCP=AddNode<UMaterialExpressionVectorParameter>(M,-1000,300);if(RCP){RCP->ParameterName=FName(TEXT("RimColor"));RCP->DefaultValue=RC;}
			auto* RPP=AddNode<UMaterialExpressionScalarParameter>(M,-1000,600);if(RPP){RPP->ParameterName=FName(TEXT("RimPower"));RPP->DefaultValue=RP;}
			auto* RIP=AddNode<UMaterialExpressionScalarParameter>(M,-1000,800);if(RIP){RIP->ParameterName=FName(TEXT("RimIntensity"));RIP->DefaultValue=RI;}
			auto* Fe=AddNode<UMaterialExpressionFresnel>(M,-800,400);
			auto* MF=AddNode<UMaterialExpressionMultiply>(M,-600,400);if(MF&&Fe&&RIP){Conn(Fe,TEXT(""),MF,TEXT("A"));Conn(RIP,TEXT(""),MF,TEXT("B"));}
			auto* MC=AddNode<UMaterialExpressionMultiply>(M,-400,300);if(MC&&RCP&&MF){Conn(RCP,TEXT("RGB"),MC,TEXT("A"));Conn(MF,TEXT(""),MC,TEXT("B"));}
			auto* Ad=AddNode<UMaterialExpressionAdd>(M,-200,0);if(Ad&&BCP&&MC){Conn(BCP,TEXT("RGB"),Ad,TEXT("A"));Conn(MC,TEXT("RGB"),Ad,TEXT("B"));ConnProp(Ad,TEXT("RGB"),MP_EmissiveColor);}
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}Su=FString::Printf(TEXT("Created fresnel rim: %s"),*P);return true;}
	,nullptr,5});

	// ==== material_create_wind ====
	R.Register({TEXT("material_create_wind"),
		TEXT("Foliage wind: Time+sin/cos -> WPO via Custom node. Subtle vertex sway for grass/trees."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("wind_speed"),SB::Number(TEXT("1.0"))},{TEXT("wind_amplitude"),SB::Number(TEXT("5.0"))},
			{TEXT("base_color"),SB::String(TEXT("Hex '#228B22'"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;
			FString H;FLinearColor BC=Hex(TEXT("228B22"));if(A->TryGetStringField(TEXT("base_color"),H)&&!H.IsEmpty())BC=Hex(H);
			float WS=A->HasTypedField<EJson::Number>(TEXT("wind_speed"))?(float)A->GetNumberField(TEXT("wind_speed")):1.f;
			float WA=A->HasTypedField<EJson::Number>(TEXT("wind_amplitude"))?(float)A->GetNumberField(TEXT("wind_amplitude")):5.f;
			auto* BCP=AddNode<UMaterialExpressionVectorParameter>(M,-800,0);if(BCP){BCP->ParameterName=FName(TEXT("BaseColor"));BCP->DefaultValue=BC;ConnProp(BCP,TEXT(""),MP_BaseColor);}
			auto* WSP=AddNode<UMaterialExpressionScalarParameter>(M,-800,300);if(WSP){WSP->ParameterName=FName(TEXT("WindSpeed"));WSP->DefaultValue=WS;}
			auto* WAP=AddNode<UMaterialExpressionScalarParameter>(M,-800,500);if(WAP){WAP->ParameterName=FName(TEXT("WindAmplitude"));WAP->DefaultValue=WA;}
			auto* Ti=AddNode<UMaterialExpressionTime>(M,-600,300);
			auto* Cu=AddNode<UMaterialExpressionCustom>(M,0,300);
			if(Cu){Cu->Code=TEXT("return float3(sin(Time*Speed)*Amplitude, 0.0, cos(Time*Speed*0.7)*Amplitude*0.5);");
				Cu->OutputType=ECustomMaterialOutputType::CMOT_Float3;
				FCustomInput CI1;CI1.InputName=FName(TEXT("Time"));
				FCustomInput CI2;CI2.InputName=FName(TEXT("Speed"));
				FCustomInput CI3;CI3.InputName=FName(TEXT("Amplitude"));
				Cu->Inputs.Add(CI1);Cu->Inputs.Add(CI2);Cu->Inputs.Add(CI3);}
			if(Cu&&Ti)Conn(Ti,TEXT(""),Cu,TEXT("Time"));if(Cu&&WSP)Conn(WSP,TEXT(""),Cu,TEXT("Speed"));if(Cu&&WAP)Conn(WAP,TEXT(""),Cu,TEXT("Amplitude"));
			if(Cu)ConnProp(Cu,TEXT(""),MP_WorldPositionOffset);
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}Su=FString::Printf(TEXT("Created wind: %s"),*P);return true;}
	,nullptr,5});
	// ==== material_diagnose ====
	R.Register({TEXT("material_diagnose"),
		TEXT("Diagnose material issues: disconnected properties, missing textures, compile errors, invalid references, orphaned nodes."),
		SB::Object({{TEXT("asset_path"),SB::String(TEXT("Material path to diagnose"))},{TEXT("auto_fix_preview"),SB::Boolean(TEXT("Preview fixes without applying (default true)"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path;if(!A->TryGetStringField(TEXT("asset_path"),Path)){Er=TEXT("Missing asset_path");return false;}
			bool bPreview=!A->HasTypedField<EJson::Boolean>(TEXT("auto_fix_preview"))||A->GetBoolField(TEXT("auto_fix_preview"));
			UMaterial* M=LoadMat(Cx.Services,Path,Er);if(!M)return false;
			TArray<TSharedPtr<FJsonValue>> Issues;
			auto AddIssue=[&](const FString& Sev,const FString& Cat,const FString& Msg,const FString& Fix){
				auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("severity"),Sev);J->SetStringField(TEXT("category"),Cat);
				J->SetStringField(TEXT("message"),Msg);J->SetStringField(TEXT("suggested_fix"),Fix);Issues.Add(MakeShared<FJsonValueObject>(J));};
			// 1. Check essential properties connected
			TArray<EMaterialProperty> Props={MP_BaseColor,MP_Metallic,MP_Roughness,MP_Normal};
			TArray<FString> PropNames={TEXT("BaseColor"),TEXT("Metallic"),TEXT("Roughness"),TEXT("Normal")};
			for(int32 i=0;i<Props.Num();i++){
				if(!M->IsPropertyConnected(Props[i])){
					AddIssue(TEXT("warning"),TEXT("disconnected_property"),
						FString::Printf(TEXT("Property '%s' is not connected"),*PropNames[i]),
						FString::Printf(TEXT("Connect a node to the %s input"),*PropNames[i]));}}
			// 2. Check expressions for missing texture refs
			for(UMaterialExpression* Expr : M->GetExpressions()){
				if(auto* TSP=Cast<UMaterialExpressionTextureSampleParameter2D>(Expr)){
					if(!TSP->Texture||TSP->Texture->IsValidLowLevel()==false){
						AddIssue(TEXT("error"),TEXT("missing_texture"),
							FString::Printf(TEXT("Texture param '%s' has missing/invalid texture"),*TSP->ParameterName.ToString()),
							TEXT("Assign a valid texture asset"));}}}
			// 3. Check blend mode consistency
			if(M->GetBlendMode()==BLEND_Translucent&&!M->IsPropertyConnected(MP_Opacity)){
				AddIssue(TEXT("error"),TEXT("blend_mode"),TEXT("Translucent material has no Opacity connected"),TEXT("Connect a value to Opacity"));}
			if(M->GetBlendMode()==BLEND_Masked&&!M->IsPropertyConnected(MP_OpacityMask)){
				AddIssue(TEXT("error"),TEXT("blend_mode"),TEXT("Masked material has no OpacityMask connected"),TEXT("Connect a value to OpacityMask"));}
			O->SetArrayField(TEXT("issues"),Issues);
			O->SetNumberField(TEXT("issue_count"),Issues.Num());
			int32 Errors=0,Warnings=0;
			for(auto& V:Issues){auto J=V->AsObject();if(J->GetStringField(TEXT("severity"))==TEXT("error"))Errors++;else Warnings++;}
			O->SetNumberField(TEXT("errors"),Errors);O->SetNumberField(TEXT("warnings"),Warnings);
			O->SetStringField(TEXT("asset_path"),Path);
			O->SetStringField(TEXT("status"),bPreview?TEXT("preview"):TEXT("diagnosed"));
			Su=FString::Printf(TEXT("Diagnosed %s: %d errors, %d warnings, %d total issues"),*Path,Errors,Warnings,Issues.Num());
			return true;}
	,nullptr,5});

	// ==== material_repair ====
	R.Register({TEXT("material_repair"),
		TEXT("Auto-repair material: reconnect defaults, fix missing textures, remove orphaned nodes, resolve compile errors."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("fixes"),SB::String(TEXT("Comma-separated: disconnected,missing_texture,orphaned,blend_mode. Default: all"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path;if(!A->TryGetStringField(TEXT("asset_path"),Path)){Er=TEXT("Missing asset_path");return false;}
			FString FixesStr;if(!A->TryGetStringField(TEXT("fixes"),FixesStr)||FixesStr.IsEmpty())FixesStr=TEXT("all");
			bool bAll=FixesStr==TEXT("all");
			TArray<FString> Fixes;if(!bAll)FixesStr.ParseIntoArray(Fixes,TEXT(","),true);
			auto HasFix=[&](const FString& F){return bAll||Fixes.ContainsByPredicate([&](const FString& S){return S.TrimStartAndEnd()==F;});};
			UMaterial* M=LoadMat(Cx.Services,Path,Er);if(!M)return false;
			const FScopedTransaction Trans(NSLOCTEXT("SOMOLMCP","MatRepair","Repair Material"));
			TArray<TSharedPtr<FJsonValue>> Applied;
			auto Log=[&](const FString& F,const FString& Msg){auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("fix"),F);J->SetStringField(TEXT("message"),Msg);Applied.Add(MakeShared<FJsonValueObject>(J));};
			M->PreEditChange(nullptr);
			// 1. Fix disconnected properties
			if(HasFix(TEXT("disconnected"))){
				if(!M->IsPropertyConnected(MP_BaseColor)){
					auto* V=AddNode<UMaterialExpressionVectorParameter>(M,-400,0);
					if(V){V->ParameterName=FName(TEXT("BaseColor"));V->DefaultValue=FLinearColor(0.5f,0.5f,0.5f);ConnProp(V,TEXT(""),MP_BaseColor);Log(TEXT("disconnected"),TEXT("Added default gray BaseColor"));}}
				if(!M->IsPropertyConnected(MP_Metallic)){
					auto* S=AddNode<UMaterialExpressionConstant>(M,-400,200);if(S){S->R=0.f;ConnProp(S,TEXT(""),MP_Metallic);Log(TEXT("disconnected"),TEXT("Added default Metallic=0"));}}
				if(!M->IsPropertyConnected(MP_Roughness)){
					auto* S=AddNode<UMaterialExpressionConstant>(M,-400,400);if(S){S->R=0.5f;ConnProp(S,TEXT(""),MP_Roughness);Log(TEXT("disconnected"),TEXT("Added default Roughness=0.5"));}}
				if(M->GetBlendMode()==BLEND_Translucent&&!M->IsPropertyConnected(MP_Opacity)){
					auto* S=AddNode<UMaterialExpressionConstant>(M,-400,600);if(S){S->R=0.5f;ConnProp(S,TEXT(""),MP_Opacity);Log(TEXT("disconnected"),TEXT("Added Opacity=0.5 for translucent"));}}
				if(M->GetBlendMode()==BLEND_Masked&&!M->IsPropertyConnected(MP_OpacityMask)){
					auto* S=AddNode<UMaterialExpressionConstant>(M,-400,800);if(S){S->R=1.f;ConnProp(S,TEXT(""),MP_OpacityMask);Log(TEXT("disconnected"),TEXT("Added OpacityMask=1 for masked"));}}}
			// 2. Fix blend mode
			if(HasFix(TEXT("blend_mode"))){
				if(M->GetBlendMode()==BLEND_Translucent&&!M->IsPropertyConnected(MP_Opacity)){
					M->BlendMode=BLEND_Opaque;Log(TEXT("blend_mode"),TEXT("Changed Translucent->Opaque (no Opacity)"));}
				if(M->GetBlendMode()==BLEND_Masked&&!M->IsPropertyConnected(MP_OpacityMask)){
					M->BlendMode=BLEND_Opaque;Log(TEXT("blend_mode"),TEXT("Changed Masked->Opaque (no OpacityMask)"));}
			}
			M->PostEditChange();
			if(Applied.Num()==0){
				SololmcpError::Set(O,TEXT("NO_OP"),TEXT("fixes"),TEXT("No requested repair produced a material change."));
				O->SetStringField(TEXT("asset_path"),Path);
				O->SetNumberField(TEXT("fixes_applied"),0);
				Er=TEXT("No repair fixes were applied.");
				return false;}
			if(!Cx.Services.SaveAsset(Path,false,Er)){
				SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);
				return false;}
			FString ReloadErr;UMaterial* Reloaded=Cast<UMaterial>(Cx.Services.LoadAsset(Path,ReloadErr));
			if(!Reloaded){
				SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),TEXT("Material failed reload validation after repair."));
				Er=ReloadErr;
				return false;}
			O->SetArrayField(TEXT("applied_fixes"),Applied);
			O->SetNumberField(TEXT("fixes_applied"),Applied.Num());
			O->SetStringField(TEXT("asset_path"),Path);
			Su=FString::Printf(TEXT("Repaired %s: %d fixes applied"),*Path,Applied.Num());return true;}
	,nullptr,5});
	// ==== material_create_animated_uv ====
		R.Register({TEXT("material_create_animated_uv"),
		TEXT("Animated UV: Panner(Time,SpeedX,SpeedY) → TextureSample. Scrolling texture effect. Texture optional (defaults to /Engine/EngineResources/DefaultDiffuse)."),
		SB::Object({{TEXT("asset_path"),SB::String()},{TEXT("texture"),SB::String(TEXT("Texture path (optional, defaults to DefaultDiffuse)"))},
			{TEXT("speed_x"),SB::Number(TEXT("0.1"))},{TEXT("speed_y"),SB::Number(TEXT("0.0"))},
			{TEXT("rotation_speed"),SB::Number(TEXT("Optional: rotation speed (sets speed_x, speed_y=0)"))}
		},{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Path,Pkg,Nam;if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
			UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);if(!M)return false;
			FString TPath;if(!A->TryGetStringField(TEXT("texture"),TPath)||TPath.IsEmpty()){TPath=TEXT("/Engine/EngineResources/DefaultDiffuse.DefaultDiffuse");}
			float SX=0.1f,SY=0.f;
			if(A->HasTypedField<EJson::Number>(TEXT("speed_x")))SX=(float)A->GetNumberField(TEXT("speed_x"));
			else if(A->HasTypedField<EJson::Number>(TEXT("rotation_speed")))SX=(float)A->GetNumberField(TEXT("rotation_speed"));
			if(A->HasTypedField<EJson::Number>(TEXT("speed_y")))SY=(float)A->GetNumberField(TEXT("speed_y"));
			auto* UV=AddNode<UMaterialExpressionTextureCoordinate>(M,-1200,0);
			auto* Ti=AddNode<UMaterialExpressionTime>(M,-1200,300);
			auto* SP=AddNode<UMaterialExpressionScalarParameter>(M,-1200,600);if(SP){SP->ParameterName=FName(TEXT("SpeedX"));SP->DefaultValue=SX;}
			auto* SP2=AddNode<UMaterialExpressionScalarParameter>(M,-1200,800);if(SP2){SP2->ParameterName=FName(TEXT("SpeedY"));SP2->DefaultValue=SY;}
			auto* M1=AddNode<UMaterialExpressionMultiply>(M,-1000,300);if(M1&&Ti&&SP){Conn(Ti,TEXT(""),M1,TEXT("A"));Conn(SP,TEXT(""),M1,TEXT("B"));}
			auto* M2=AddNode<UMaterialExpressionMultiply>(M,-1000,600);if(M2&&Ti&&SP2){Conn(Ti,TEXT(""),M2,TEXT("A"));Conn(SP2,TEXT(""),M2,TEXT("B"));}
			auto* Ad=AddNode<UMaterialExpressionAdd>(M,-800,0);
			if(Ad&&UV&&M1&&M2){Conn(UV,TEXT("RG"),Ad,TEXT("A"));Conn(M1,TEXT(""),Ad,TEXT("B"));}
			auto* TS=AddNode<UMaterialExpressionTextureSampleParameter2D>(M,-400,0);
			if(TS){TS->ParameterName=FName(TEXT("AnimatedTexture"));if(auto* Tx=LoadObject<UTexture>(nullptr,*TPath))TS->Texture=Tx;
				if(Ad)Conn(Ad,TEXT(""),TS,TEXT("UVs"));ConnProp(TS,TEXT("RGB"),MP_BaseColor);}
			FString P=GetPath(M);if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}Su=FString::Printf(TEXT("Created animated UV: %s"),*P);return true;}
	,nullptr,5});

	// ==== material_assign_to_selected ====
	R.Register({TEXT("material_assign_to_selected"),
		TEXT("Assign a Material or MaterialInstance to actors' mesh components. Use actor_names to target specific actors, or omit to use editor selection. Supports StaticMesh and SkeletalMesh."),
		SB::Object({
			{TEXT("material_path"),SB::String(TEXT("Material asset path"))},
			{TEXT("actor_names"),SB::Array(SB::String(TEXT("Actor label names (optional, uses selection if omitted)")))},
			{TEXT("material_index"),SB::Number(TEXT("Slot index (0-based)"))}
		},{TEXT("material_path")}),
		[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString MatPath;if(!A->TryGetStringField(TEXT("material_path"),MatPath)){Er=TEXT("Missing material_path");return false;}
			int32 Slot=A->HasTypedField<EJson::Number>(TEXT("material_index"))?(int32)A->GetNumberField(TEXT("material_index")):0;
			UMaterialInterface* Mat=LoadMaterialInterface(Cx.Services,MatPath,Er);if(!Mat)return false;
			UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){Er=TEXT("No world");return false;}
			TArray<AActor*> Targets;
			// Try actor_names first — 3-tier matching (PathName > GetName > ActorLabel)
			const TArray<TSharedPtr<FJsonValue>>*Names=nullptr;
			if(A->TryGetArrayField(TEXT("actor_names"),Names)&&Names->Num()>0){
				for(const auto&V:*Names){
					FString Nm=V->AsString();
					AActor*Found=nullptr;
					// Tier 1: PathName
					for(TActorIterator<AActor>It(W);It&&!Found;++It){if((*It)->GetPathName()==Nm)Found=*It;}
					// Tier 2: GetName
					if(!Found){for(TActorIterator<AActor>It(W);It&&!Found;++It){if((*It)->GetName()==Nm)Found=*It;}}
					// Tier 3: ActorLabel
					if(!Found){for(TActorIterator<AActor>It(W);It&&!Found;++It){if((*It)->GetActorLabel()==Nm)Found=*It;}}
					if(Found)Targets.Add(Found);
				}
			}else{
				// Fallback to editor selection
				TArray<UObject*>SelObjs;USelection*Sel=GEditor->GetSelectedActors();
				if(Sel)for(FSelectionIterator It(*Sel);It;++It)if(auto*O=Cast<AActor>(*It))Targets.Add(O);
			}
			if(Targets.Num()==0){Er=TEXT("No actors found (provide actor_names or select in editor)");return false;}
			int32 Applied=0;
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP","MaterialAssignInterface","SOMOLMCP Assign Material Interface"));
			for(auto*Actor:Targets){
				TArray<UMeshComponent*>Meshes;Actor->GetComponents<UMeshComponent>(Meshes);
				for(auto*Mesh:Meshes){
					if(Slot>=0&&Slot<Mesh->GetNumMaterials()){
						Mesh->Modify();
						Mesh->SetMaterial(Slot,Mat);
						if(Mesh->GetMaterial(Slot)==Mat){
							Mesh->MarkPackageDirty();
							Applied++;
						}}}}
			if(Applied==0){
				SololmcpError::Set(O,TEXT("NO_OP"),TEXT("material_index"),TEXT("No mesh material slots matched the requested material_index."));
				O->SetStringField(TEXT("material_path"),MatPath);O->SetNumberField(TEXT("applied_count"),0);
				Er=TEXT("No material slots were updated.");
				return false;}
			O->SetStringField(TEXT("material_path"),MatPath);O->SetNumberField(TEXT("applied_count"),Applied);
			O->SetStringField(TEXT("material_class"),Mat->GetClass()->GetPathName());
			O->SetBoolField(TEXT("readback_verified"),true);
			TArray<FString>Labels;for(auto*T:Targets)Labels.Add(T->GetActorLabel());
			O->SetStringField(TEXT("actors"),FString::Join(Labels,TEXT(", ")));
			Su=FString::Printf(TEXT("Applied %s to %d slots on %d actors"),*MatPath,Applied,Targets.Num());return true;}
	,nullptr,5});
	// TEMPLATE_TOOLS_END

	// ─────────────────────────────────────────────────────────────────────────
	// Short-name aliases (from former SololmcpMaterialTools.cpp.disabled).
	// All route to UMaterialEditingLibrary on a UMaterialInstanceConstant.
	// Names preserved for backwards compatibility with older skills/DAGs.
	// ─────────────────────────────────────────────────────────────────────────

	auto LoadMIC=[](FSololmcpEditorServices& S,const FString& P,FString& E)->UMaterialInstanceConstant*{
		UObject* A=S.LoadAsset(P,E); if(!A) return nullptr;
		UMaterialInstanceConstant* MI=Cast<UMaterialInstanceConstant>(A);
		if(!MI){ E=TEXT("Asset is not a MaterialInstanceConstant."); return nullptr; }
		return MI;
	};

	// material_set_scalar_param — alias of material_instance_set_scalar_parameter.
	R.Register({TEXT("material_set_scalar_param"),
		TEXT("[Alias] Set a scalar parameter on a MaterialInstanceConstant."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("MI asset path"))},
			{TEXT("parameter_name"),FSololmcpSchemaBuilder::String()},
			{TEXT("value"),FSololmcpSchemaBuilder::Number()}},
			{TEXT("asset_path"),TEXT("parameter_name"),TEXT("value")}),
		[LoadMIC](const FSololmcpToolExecutionContext& Ctx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString P=A->GetStringField(TEXT("asset_path"));
			UMaterialInstanceConstant* MI=LoadMIC(Ctx.Services,P,Er); if(!MI) return false;
			FString Pn=A->GetStringField(TEXT("parameter_name"));
			double V=0.0; if(!A->TryGetNumberField(TEXT("value"),V)){Er=TEXT("Missing numeric 'value'.");return false;}
			const bool bOk=UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI,*Pn,static_cast<float>(V));
			if(!bOk){Er=FString::Printf(TEXT("Parameter '%s' not found on MI."),*Pn);return false;}
			UMaterialEditingLibrary::UpdateMaterialInstance(MI);
			float Verify=0.f;
			if(!MI->GetScalarParameterValue(FMaterialParameterInfo(FName(*Pn)),Verify)||!FMath::IsNearlyEqual(Verify,static_cast<float>(V))){
				SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("parameter_name"),TEXT("Scalar parameter setter did not read back the requested value."));
				Er=FString::Printf(TEXT("Scalar parameter '%s' did not verify."),*Pn);
				return false;}
			MI->MarkPackageDirty();SololmcpWriteFlush::EnsureFlushed(MI);
			if(!Ctx.Services.SaveAsset(P,false,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}
			O->SetStringField(TEXT("asset_path"),P); O->SetStringField(TEXT("parameter"),Pn); O->SetNumberField(TEXT("value"),V);
			Su=FString::Printf(TEXT("Scalar '%s'=%f on %s"),*Pn,V,*P); return true;
		}
	,nullptr,0});

	// material_set_vector_param — alias of material_instance_set_vector_parameter (RGBA).
	R.Register({TEXT("material_set_vector_param"),
		TEXT("[Alias] Set a vector (FLinearColor) parameter on a MaterialInstanceConstant."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),FSololmcpSchemaBuilder::String()},
			{TEXT("parameter_name"),FSololmcpSchemaBuilder::String()},
			{TEXT("r"),FSololmcpSchemaBuilder::Number()},
			{TEXT("g"),FSololmcpSchemaBuilder::Number()},
			{TEXT("b"),FSololmcpSchemaBuilder::Number()},
			{TEXT("a"),FSololmcpSchemaBuilder::Number(TEXT("alpha (default 1)"))}},
			{TEXT("asset_path"),TEXT("parameter_name"),TEXT("r"),TEXT("g"),TEXT("b")}),
		[LoadMIC](const FSololmcpToolExecutionContext& Ctx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString P=A->GetStringField(TEXT("asset_path"));
			UMaterialInstanceConstant* MI=LoadMIC(Ctx.Services,P,Er); if(!MI) return false;
			FString Pn=A->GetStringField(TEXT("parameter_name"));
			double R=0,G=0,B=0,Al=1.0;
			A->TryGetNumberField(TEXT("r"),R); A->TryGetNumberField(TEXT("g"),G);
			A->TryGetNumberField(TEXT("b"),B); A->TryGetNumberField(TEXT("a"),Al);
			const FLinearColor C((float)R,(float)G,(float)B,(float)Al);
			const bool bOk=UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MI,*Pn,C);
			if(!bOk){Er=FString::Printf(TEXT("Vector param '%s' not found."),*Pn);return false;}
			UMaterialEditingLibrary::UpdateMaterialInstance(MI);
			FLinearColor Verify=FLinearColor::Black;
			if(!MI->GetVectorParameterValue(FMaterialParameterInfo(FName(*Pn)),Verify)||!Verify.Equals(C,KINDA_SMALL_NUMBER)){
				SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("parameter_name"),TEXT("Vector parameter setter did not read back the requested value."));
				Er=FString::Printf(TEXT("Vector parameter '%s' did not verify."),*Pn);
				return false;}
			MI->MarkPackageDirty();SololmcpWriteFlush::EnsureFlushed(MI);
			if(!Ctx.Services.SaveAsset(P,false,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}
			O->SetStringField(TEXT("asset_path"),P); O->SetStringField(TEXT("parameter"),Pn);
			TSharedPtr<FJsonObject> Cv=MakeShared<FJsonObject>();
			Cv->SetNumberField(TEXT("r"),R);Cv->SetNumberField(TEXT("g"),G);Cv->SetNumberField(TEXT("b"),B);Cv->SetNumberField(TEXT("a"),Al);
			O->SetObjectField(TEXT("value"),Cv);
			Su=FString::Printf(TEXT("Vector '%s' on %s"),*Pn,*P); return true;
		}
	,nullptr,0});

	// material_set_texture_param — alias of material_instance_set_texture_parameter.
	R.Register({TEXT("material_set_texture_param"),
		TEXT("[Alias] Set a texture parameter on a MaterialInstanceConstant."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),FSololmcpSchemaBuilder::String()},
			{TEXT("parameter_name"),FSololmcpSchemaBuilder::String()},
			{TEXT("texture_path"),FSololmcpSchemaBuilder::String()}},
			{TEXT("asset_path"),TEXT("parameter_name"),TEXT("texture_path")}),
		[LoadMIC](const FSololmcpToolExecutionContext& Ctx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString P=A->GetStringField(TEXT("asset_path"));
			UMaterialInstanceConstant* MI=LoadMIC(Ctx.Services,P,Er); if(!MI) return false;
			FString Pn=A->GetStringField(TEXT("parameter_name"));
			FString Tp=A->GetStringField(TEXT("texture_path"));
			UObject* TexObj=Ctx.Services.LoadAsset(Tp,Er); if(!TexObj) return false;
			UTexture* Tex=Cast<UTexture>(TexObj);
			if(!Tex){Er=TEXT("texture_path does not resolve to a UTexture.");return false;}
			const bool bOk=UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MI,*Pn,Tex);
			if(!bOk){Er=FString::Printf(TEXT("Texture param '%s' not found."),*Pn);return false;}
			UMaterialEditingLibrary::UpdateMaterialInstance(MI);
			UTexture* Verify=nullptr;
			if(!MI->GetTextureParameterValue(FMaterialParameterInfo(FName(*Pn)),Verify)||Verify!=Tex){
				SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("parameter_name"),TEXT("Texture parameter setter did not read back the requested texture."));
				Er=FString::Printf(TEXT("Texture parameter '%s' did not verify."),*Pn);
				return false;}
			MI->MarkPackageDirty();SololmcpWriteFlush::EnsureFlushed(MI);
			if(!Ctx.Services.SaveAsset(P,false,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}
			O->SetStringField(TEXT("asset_path"),P); O->SetStringField(TEXT("parameter"),Pn); O->SetStringField(TEXT("texture"),Tp);
			Su=FString::Printf(TEXT("Texture '%s'=%s"),*Pn,*Tp); return true;
		}
	,nullptr,0});

	// material_get_all_params — read-only: list all scalar/vector/texture params on MI.
	R.Register({TEXT("material_get_all_params"),
		TEXT("[Alias] Get all scalar/vector/texture parameters on a MaterialInstanceConstant."),
		FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String()}},{TEXT("asset_path")}),
		[LoadMIC](const FSololmcpToolExecutionContext& Ctx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString P=A->GetStringField(TEXT("asset_path"));
			UMaterialInstanceConstant* MI=LoadMIC(Ctx.Services,P,Er); if(!MI) return false;
			TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
			TArray<TSharedPtr<FJsonValue>> SA,VA,TA;
			MI->GetAllScalarParameterInfo(Infos,Guids);
			for(const FMaterialParameterInfo& I:Infos){
				float Val=0; MI->GetScalarParameterValue(I,Val);
				TSharedPtr<FJsonObject> Po=MakeShared<FJsonObject>();
				Po->SetStringField(TEXT("name"),I.Name.ToString());Po->SetNumberField(TEXT("value"),Val);
				SA.Add(MakeShared<FJsonValueObject>(Po));
			}
			Infos.Reset();Guids.Reset();
			MI->GetAllVectorParameterInfo(Infos,Guids);
			for(const FMaterialParameterInfo& I:Infos){
				FLinearColor C=FLinearColor::Black; MI->GetVectorParameterValue(I,C);
				TSharedPtr<FJsonObject> Po=MakeShared<FJsonObject>();
				Po->SetStringField(TEXT("name"),I.Name.ToString());
				Po->SetNumberField(TEXT("r"),C.R);Po->SetNumberField(TEXT("g"),C.G);
				Po->SetNumberField(TEXT("b"),C.B);Po->SetNumberField(TEXT("a"),C.A);
				VA.Add(MakeShared<FJsonValueObject>(Po));
			}
			Infos.Reset();Guids.Reset();
			MI->GetAllTextureParameterInfo(Infos,Guids);
			for(const FMaterialParameterInfo& I:Infos){
				UTexture* Tx=nullptr; MI->GetTextureParameterValue(I,Tx);
				TSharedPtr<FJsonObject> Po=MakeShared<FJsonObject>();
				Po->SetStringField(TEXT("name"),I.Name.ToString());
				Po->SetStringField(TEXT("texture"),Tx?Tx->GetPathName():FString());
				TA.Add(MakeShared<FJsonValueObject>(Po));
			}
			O->SetArrayField(TEXT("scalars"),SA);
			O->SetArrayField(TEXT("vectors"),VA);
			O->SetArrayField(TEXT("textures"),TA);
			Su=FString::Printf(TEXT("%d scalars, %d vectors, %d textures"),SA.Num(),VA.Num(),TA.Num());
			return true;
		}
	,nullptr,10});

	// material_duplicate — duplicate a UMaterial asset to a new path.
	R.Register({TEXT("material_duplicate"),
		TEXT("[Alias] Duplicate a UMaterial asset to a new path/name."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("source_path"),FSololmcpSchemaBuilder::String(TEXT("Source material asset path"))},
			{TEXT("dest_path"),FSololmcpSchemaBuilder::String(TEXT("Destination package path, e.g. /Game/Materials"))},
			{TEXT("dest_name"),FSololmcpSchemaBuilder::String(TEXT("New asset name"))}},
			{TEXT("source_path"),TEXT("dest_path"),TEXT("dest_name")}),
		[](const FSololmcpToolExecutionContext& Ctx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
			FString Sp=A->GetStringField(TEXT("source_path"));
			FString Dp=A->GetStringField(TEXT("dest_path"));
			FString Dn=A->GetStringField(TEXT("dest_name"));
			const FString UniqueName=Ctx.Services.GenerateUniqueAssetName(Dp,Dn);
			const FString DestFullPath=Dp+TEXT("/")+UniqueName;
			UObject* Dup=Ctx.Services.DuplicateAsset(Sp,DestFullPath,Er);
			if(!Dup) return false;
			if(!Ctx.Services.AssetExists(Dup->GetPathName())){
				SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("dest_path"),TEXT("DuplicateAsset returned an object but the asset registry cannot find it."));
				Er=FString::Printf(TEXT("Duplicated asset was not persisted: %s"),*Dup->GetPathName());
				return false;}
			FString ReloadErr;UObject* Reloaded=Ctx.Services.LoadAsset(Dup->GetPathName(),ReloadErr);
			if(!Reloaded||!Reloaded->IsA(Dup->GetClass())){
				SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("dest_path"),TEXT("Duplicated asset failed reload/class validation."));
				Er=FString::Printf(TEXT("Duplicated asset failed reload validation: %s"),*ReloadErr);
				return false;}
			O->SetStringField(TEXT("source"),Sp);
			O->SetStringField(TEXT("path"),Dup->GetPathName());
			Su=FString::Printf(TEXT("Duplicated %s -> %s"),*Sp,*Dup->GetPathName());
			return true;
		}
	,nullptr,0});

	// ═══════════════════════════════════════════════════════════════════════
	// FIX 24-26 (v12-rd5): material asset-level property tools
	// ═══════════════════════════════════════════════════════════════════════
	// material_set_attributes — set BlendMode/ShadingModel/TwoSided/OpacityMaskClipValue
	// material_inspect_attributes — read those same properties
	// material_create_foliage  — convenience: PBR + Masked + Foliage shading + TwoSided
	// (These are the missing tools that prevented building tree alpha-test materials.)
	// ═══════════════════════════════════════════════════════════════════════

	R.Register({TEXT("material_inspect_attributes"),
	TEXT("Read asset-level material properties: blend_mode, shading_model, two_sided, opacity_mask_clip_value, dithered_lod, used_as_special_engine_material, domain."),
	SB::Object({{TEXT("asset_path"),SB::String(TEXT("Material asset path"))}},{TEXT("asset_path")}),
	[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
		FString Path;
		if(!A->TryGetStringField(TEXT("asset_path"),Path)||Path.IsEmpty()){
			SololmcpError::MissingParam(O,TEXT("asset_path"));
			Er=TEXT("Missing asset_path.");
			return false;
		}
		UMaterial* M=Cast<UMaterial>(Cx.Services.LoadAsset(Path,Er));
		if(!M){
			SololmcpError::InvalidPath(O,Path);
			Er=TEXT("Material not found: ")+Path;
			return false;
		}
		const TCHAR* BlendStr=TEXT("Opaque");
		switch(M->BlendMode){
			case BLEND_Opaque: BlendStr=TEXT("Opaque"); break;
			case BLEND_Masked: BlendStr=TEXT("Masked"); break;
			case BLEND_Translucent: BlendStr=TEXT("Translucent"); break;
			case BLEND_Additive: BlendStr=TEXT("Additive"); break;
			case BLEND_Modulate: BlendStr=TEXT("Modulate"); break;
			case BLEND_AlphaComposite: BlendStr=TEXT("AlphaComposite"); break;
			case BLEND_AlphaHoldout: BlendStr=TEXT("AlphaHoldout"); break;
			default: BlendStr=TEXT("?"); break;
		}
		const TCHAR* ShadeStr=TEXT("DefaultLit");
		const FMaterialShadingModelField& SM=M->GetShadingModels();
		if(SM.HasShadingModel(MSM_Unlit)) ShadeStr=TEXT("Unlit");
		else if(SM.HasShadingModel(MSM_Subsurface)) ShadeStr=TEXT("Subsurface");
		else if(SM.HasShadingModel(MSM_PreintegratedSkin)) ShadeStr=TEXT("PreintegratedSkin");
		else if(SM.HasShadingModel(MSM_ClearCoat)) ShadeStr=TEXT("ClearCoat");
		else if(SM.HasShadingModel(MSM_SubsurfaceProfile)) ShadeStr=TEXT("SubsurfaceProfile");
		else if(SM.HasShadingModel(MSM_TwoSidedFoliage)) ShadeStr=TEXT("TwoSidedFoliage");
		else if(SM.HasShadingModel(MSM_Hair)) ShadeStr=TEXT("Hair");
		else if(SM.HasShadingModel(MSM_Cloth)) ShadeStr=TEXT("Cloth");
		else if(SM.HasShadingModel(MSM_Eye)) ShadeStr=TEXT("Eye");
		// P1-2: report current MaterialDomain as a string
		const TCHAR* DomainStr=TEXT("surface");
		switch(M->MaterialDomain){
			case MD_Surface:        DomainStr=TEXT("surface"); break;
			case MD_DeferredDecal:  DomainStr=TEXT("deferred_decal"); break;
			case MD_PostProcess:    DomainStr=TEXT("post_process"); break;
			case MD_UI:             DomainStr=TEXT("ui"); break;
			case MD_Volume:         DomainStr=TEXT("volume"); break;
			case MD_LightFunction:  DomainStr=TEXT("light_function"); break;
			case MD_RuntimeVirtualTexture: DomainStr=TEXT("virtual_texture"); break;
			default:                DomainStr=TEXT("surface"); break;
		}
		O->SetStringField(TEXT("asset_path"),Path);
		O->SetStringField(TEXT("blend_mode"),BlendStr);
		O->SetStringField(TEXT("shading_model"),ShadeStr);
		O->SetStringField(TEXT("domain"),DomainStr);
		O->SetBoolField(TEXT("two_sided"),M->TwoSided);
		O->SetNumberField(TEXT("opacity_mask_clip_value"),M->OpacityMaskClipValue);
		O->SetBoolField(TEXT("dithered_lod_transition"),M->DitheredLODTransition);
		O->SetBoolField(TEXT("used_as_special_engine_material"),M->bUsedAsSpecialEngineMaterial);
		Su=FString::Printf(TEXT("Material '%s': blend=%s shading=%s domain=%s two_sided=%s clip=%.2f"),
			*Path,BlendStr,ShadeStr,DomainStr,M->TwoSided?TEXT("true"):TEXT("false"),M->OpacityMaskClipValue);
		return true;
	},nullptr,5});

	R.Register({TEXT("material_set_attributes"),
	TEXT("Set asset-level material properties. Pass any subset: blend_mode (Opaque/Masked/Translucent/Additive/Modulate), shading_model (DefaultLit/Unlit/Subsurface/Foliage/TwoSidedFoliage/etc), two_sided (bool), opacity_mask_clip_value (0..1), dithered_lod_transition (bool), domain (surface/decal/post_process/ui/volume/light_function). Recompiles after change."),
	SB::Object({
		{TEXT("asset_path"),SB::String(TEXT("Material asset path"))},
		{TEXT("blend_mode"),SB::String(TEXT("Opaque|Masked|Translucent|Additive|Modulate|AlphaComposite|AlphaHoldout"))},
		{TEXT("shading_model"),SB::String(TEXT("DefaultLit|Unlit|Subsurface|PreintegratedSkin|ClearCoat|SubsurfaceProfile|TwoSidedFoliage|Hair|Cloth|Eye"))},
		{TEXT("two_sided"),SB::Boolean(TEXT("True for foliage/leaves/cloth"))},
		{TEXT("opacity_mask_clip_value"),SB::Number(TEXT("0.0-1.0; alpha-test threshold (Masked only); default 0.333"))},
		{TEXT("dithered_lod_transition"),SB::Boolean(TEXT("Smooth LOD pop"))},
		{TEXT("domain"),SB::String(TEXT("surface|decal|deferred_decal|post_process|ui|volume|light_function|virtual_texture"))}
	},{TEXT("asset_path")}),
	[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
		FString Path;
		if(!A->TryGetStringField(TEXT("asset_path"),Path)||Path.IsEmpty()){
			SololmcpError::MissingParam(O,TEXT("asset_path"));
			Er=TEXT("Missing asset_path.");
			return false;
		}
		UMaterial* M=Cast<UMaterial>(Cx.Services.LoadAsset(Path,Er));
		if(!M){
			SololmcpError::InvalidPath(O,Path);
			Er=TEXT("Material not found: ")+Path;
			return false;
		}

		// FIX (v12-rd6): wrap modifications in Modify() + PostEditChange().
		// Old code called ForceRecompileForRendering() BEFORE PostEditChange,
		// which triggered async shader compile reading current (still old)
		// values, and then PostEditChange came too late to push the new values
		// back through the dirty-tracking pipeline. Result: changes appeared to
		// stick in-memory but the next inspect read the original values back.
		// New flow: Modify (mark dirty / undo) -> set fields -> PostEditChange
		// (UE handles recompile internally + persists the new BlendMode etc).
		M->Modify();

		int32 changed=0;
		FString BM;
		if(A->TryGetStringField(TEXT("blend_mode"),BM)&&!BM.IsEmpty()){
			if(BM==TEXT("Opaque")) { M->BlendMode=BLEND_Opaque; changed++; }
			else if(BM==TEXT("Masked")) { M->BlendMode=BLEND_Masked; changed++; }
			else if(BM==TEXT("Translucent")) { M->BlendMode=BLEND_Translucent; changed++; }
			else if(BM==TEXT("Additive")) { M->BlendMode=BLEND_Additive; changed++; }
			else if(BM==TEXT("Modulate")) { M->BlendMode=BLEND_Modulate; changed++; }
			else if(BM==TEXT("AlphaComposite")) { M->BlendMode=BLEND_AlphaComposite; changed++; }
			else if(BM==TEXT("AlphaHoldout")) { M->BlendMode=BLEND_AlphaHoldout; changed++; }
			else { Er=FString::Printf(TEXT("Unknown blend_mode '%s'"),*BM); return false; }
		}
		FString SM;
		if(A->TryGetStringField(TEXT("shading_model"),SM)&&!SM.IsEmpty()){
			if(SM==TEXT("DefaultLit")) { M->SetShadingModel(MSM_DefaultLit); changed++; }
			else if(SM==TEXT("Unlit")) { M->SetShadingModel(MSM_Unlit); changed++; }
			else if(SM==TEXT("Subsurface")) { M->SetShadingModel(MSM_Subsurface); changed++; }
			else if(SM==TEXT("PreintegratedSkin")) { M->SetShadingModel(MSM_PreintegratedSkin); changed++; }
			else if(SM==TEXT("ClearCoat")) { M->SetShadingModel(MSM_ClearCoat); changed++; }
			else if(SM==TEXT("SubsurfaceProfile")) { M->SetShadingModel(MSM_SubsurfaceProfile); changed++; }
			else if(SM==TEXT("TwoSidedFoliage")) { M->SetShadingModel(MSM_TwoSidedFoliage); changed++; }
			else if(SM==TEXT("Foliage")) { M->SetShadingModel(MSM_TwoSidedFoliage); changed++; }
			else if(SM==TEXT("Hair")) { M->SetShadingModel(MSM_Hair); changed++; }
			else if(SM==TEXT("Cloth")) { M->SetShadingModel(MSM_Cloth); changed++; }
			else if(SM==TEXT("Eye")) { M->SetShadingModel(MSM_Eye); changed++; }
			else { Er=FString::Printf(TEXT("Unknown shading_model '%s'"),*SM); return false; }
		}
		bool TS;
		if(A->TryGetBoolField(TEXT("two_sided"),TS)){ M->TwoSided=TS; changed++; }
		double Cv;
		if(A->TryGetNumberField(TEXT("opacity_mask_clip_value"),Cv)){ M->OpacityMaskClipValue=(float)FMath::Clamp(Cv,0.0,1.0); changed++; }
		bool Dith;
		if(A->TryGetBoolField(TEXT("dithered_lod_transition"),Dith)){ M->DitheredLODTransition=Dith; changed++; }

		// P1-2: optional Material Domain
		FString Dom;
		if(A->TryGetStringField(TEXT("domain"),Dom)&&!Dom.IsEmpty()){
			const FString DLow=Dom.ToLower();
			if(DLow==TEXT("surface")) { M->MaterialDomain=MD_Surface; changed++; }
			else if(DLow==TEXT("decal")||DLow==TEXT("deferred_decal")) { M->MaterialDomain=MD_DeferredDecal; changed++; }
			else if(DLow==TEXT("post_process")) { M->MaterialDomain=MD_PostProcess; changed++; }
			else if(DLow==TEXT("ui")) { M->MaterialDomain=MD_UI; changed++; }
			else if(DLow==TEXT("volume")) { M->MaterialDomain=MD_Volume; changed++; }
			else if(DLow==TEXT("light_function")) { M->MaterialDomain=MD_LightFunction; changed++; }
			else if(DLow==TEXT("virtual_texture")) {
				M->MaterialDomain=MD_RuntimeVirtualTexture;
				changed++;
			}
			else {
				Er=FString::Printf(TEXT("Unknown domain '%s'"),*Dom);
				SololmcpError::Set(O,TEXT("INVALID_TYPE"),TEXT("domain"),TEXT("Use one of: surface|decal|post_process|ui|volume|light_function|virtual_texture."));
				return false;
			}
		}

		if(changed>0){
			// PostEditChange triggers UMaterial::PostEditChangeProperty for all
			// properties + marks for shader recompile internally + dirty.
			M->PostEditChange();
			M->MarkPackageDirty();
			if (M->GetOutermost()) { M->GetOutermost()->MarkPackageDirty(); }
			SololmcpWriteFlush::EnsureFlushed(M);
		}
		else{
			SololmcpError::Set(O,TEXT("NO_OP"),TEXT("asset_path"),TEXT("No writable material attributes were provided."));
			O->SetStringField(TEXT("asset_path"),Path);
			O->SetNumberField(TEXT("changed_count"),0);
			Er=TEXT("No material attributes were changed.");
			return false;
		}
		if(!Cx.Services.SaveAsset(Path,false,Er)){
			SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);
			return false;
		}
		FString ReloadErr;UMaterial* Reloaded=Cast<UMaterial>(Cx.Services.LoadAsset(Path,ReloadErr));
		if(!Reloaded){
			SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),TEXT("Material failed reload validation after attribute save."));
			Er=ReloadErr;
			return false;
		}
		// Mirror current domain in response (P1-2)
		const TCHAR* DomainStr=TEXT("surface");
		switch(M->MaterialDomain){
			case MD_Surface:        DomainStr=TEXT("surface"); break;
			case MD_DeferredDecal:  DomainStr=TEXT("deferred_decal"); break;
			case MD_PostProcess:    DomainStr=TEXT("post_process"); break;
			case MD_UI:             DomainStr=TEXT("ui"); break;
			case MD_Volume:         DomainStr=TEXT("volume"); break;
			case MD_LightFunction:  DomainStr=TEXT("light_function"); break;
			case MD_RuntimeVirtualTexture: DomainStr=TEXT("virtual_texture"); break;
			default:                DomainStr=TEXT("surface"); break;
		}
		O->SetStringField(TEXT("asset_path"),Path);
		O->SetStringField(TEXT("domain"),DomainStr);
		O->SetNumberField(TEXT("changed_count"),changed);
		Su=FString::Printf(TEXT("Material '%s': %d attribute(s) updated + recompiled (domain=%s)"),*Path,changed,DomainStr);
		return true;
	},nullptr,5});

	R.Register({TEXT("material_create_foliage"),
	TEXT("Convenience: create a foliage-quality material (TwoSidedFoliage shading + Masked blend + opacity mask). Use opacity_mask_texture for tree leaf cutouts. Without an opacity texture, falls back to a constant alpha mask."),
	SB::Object({
		{TEXT("asset_path"),SB::String()},
		{TEXT("base_color"),SB::String(TEXT("Hex base color, default green"))},
		{TEXT("opacity_mask_texture"),SB::String(TEXT("Optional /Game path of texture to use as opacity mask"))},
		{TEXT("opacity_clip_value"),SB::Number(TEXT("0..1 alpha threshold, default 0.333"))},
		{TEXT("roughness"),SB::Number(TEXT("0..1, default 0.7"))}
	},{TEXT("asset_path")}),
	[](const FSololmcpToolExecutionContext& Cx,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& Su,FString& Er)->bool{
		FString Path,Pkg,Nam;
		if(!A->TryGetStringField(TEXT("asset_path"),Path)||!SplitPath(Path,Pkg,Nam)){Er=TEXT("Bad path");return false;}
		UMaterial* M=CreateMat(Cx.Services,Pkg,Nam,Er);
		if(!M)return false;

		FString H;FLinearColor BC=FLinearColor(0.10f,0.32f,0.08f,1.0f);
		if(A->TryGetStringField(TEXT("base_color"),H)&&!H.IsEmpty())BC=Hex(H);
		float Cv=0.333f;
		double TmpCv=0;
		if(A->TryGetNumberField(TEXT("opacity_clip_value"),TmpCv))Cv=(float)FMath::Clamp(TmpCv,0.0,1.0);
		float Rv=0.7f;
		double TmpRv=0;
		if(A->TryGetNumberField(TEXT("roughness"),TmpRv))Rv=(float)FMath::Clamp(TmpRv,0.0,1.0);

		// Foliage attributes
		M->BlendMode = BLEND_Masked;
		M->SetShadingModel(MSM_TwoSidedFoliage);
		M->TwoSided = true;
		M->OpacityMaskClipValue = Cv;
		M->DitheredLODTransition = true;

		// Base color (vector parameter)
		auto* VP=AddNode<UMaterialExpressionVectorParameter>(M,-400,0);
		if(VP){VP->ParameterName=FName(TEXT("BaseColor"));VP->DefaultValue=BC;ConnProp(VP,TEXT(""),MP_BaseColor);}

		// Roughness scalar
		auto* RS=AddNode<UMaterialExpressionScalarParameter>(M,-400,200);
		if(RS){RS->ParameterName=FName(TEXT("Roughness"));RS->DefaultValue=Rv;ConnProp(RS,TEXT(""),MP_Roughness);}

		// Opacity mask: if texture provided use it, else use scalar (constant 1)
		FString TexPath;
		bool bHasTex = A->TryGetStringField(TEXT("opacity_mask_texture"),TexPath) && !TexPath.IsEmpty();
		if (bHasTex)
		{
			FString TexErr;
			UTexture* Tex = Cast<UTexture>(Cx.Services.LoadAsset(TexPath, TexErr));
			if (!Tex)
			{
				SololmcpError::InvalidPath(O, TexPath);
				Er = FString::Printf(TEXT("opacity_mask_texture is not a UTexture: %s"), *TexPath);
				return false;
			}
			auto* TX=AddNode<UMaterialExpressionTextureSampleParameter2D>(M,-800,500);
			if(TX){
				TX->ParameterName=FName(TEXT("OpacityMaskTexture"));
				TX->Texture = Tex;
				ConnProp(TX,TEXT("A"),MP_OpacityMask);
			}
		}
		else
		{
			auto* MS=AddNode<UMaterialExpressionScalarParameter>(M,-400,400);
			if(MS){MS->ParameterName=FName(TEXT("OpacityMask"));MS->DefaultValue=1.0f;ConnProp(MS,TEXT(""),MP_OpacityMask);}
		}

		FString P=GetPath(M);
		if(!Finish(Cx.Services,M,P,O,Su,Er)){SololmcpError::Set(O,TEXT("OPERATION_FAILED"),TEXT("asset_path"),Er);return false;}
		O->SetStringField(TEXT("blend_mode"),TEXT("Masked"));
		O->SetStringField(TEXT("shading_model"),TEXT("TwoSidedFoliage"));
		O->SetBoolField(TEXT("two_sided"),true);
		O->SetBoolField(TEXT("opacity_mask_texture_applied"),bHasTex);
		O->SetNumberField(TEXT("opacity_mask_clip_value"),Cv);
		Su=FString::Printf(TEXT("Created foliage material: %s (Masked + TwoSidedFoliage + clip=%.2f)"),*P,Cv);
		return true;
	},nullptr,5});
}
}
