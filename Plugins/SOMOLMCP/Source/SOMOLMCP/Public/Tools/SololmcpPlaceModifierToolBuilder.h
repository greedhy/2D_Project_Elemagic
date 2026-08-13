// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InteractiveToolBuilder.h"

#if WITH_EDITORONLY_DATA

#if SOMOLMCP_WITH_UE58_MESHPARTITION
#include "MeshPartitionPlaceModifierTool.h"
#endif

// UE 5.8's CURRENT_FILE_ID is defined by each header's .generated.h and never
// undefined at file end, so any engine header included after ours would steal
// the file id and make our UCLASS/GENERATED_BODY expand to foreign FID macros.
// Include the generated header last to keep CURRENT_FILE_ID pointing at this file.
#include "SololmcpPlaceModifierToolBuilder.generated.h"

// UE 5.8's stock UPlaceModifierToolBuilder requires UMeshPartitionComponentBackedTarget,
// but UMeshPartitionToolTarget does not implement it, so the tool can never pass the
// target-manager activation gate. This derived builder keeps the engine tool, factory,
// and target untouched and only relaxes the requirement set to the interfaces the real
// target class implements. The class is parsed only on UE 5.8+ where the base type and
// the UE58 MeshPartition plugin exist; older UHT versions reject namespaced base names.
UCLASS(Transient)
class USololmcpPlaceModifierToolBuilder : public UE::MeshPartition::UPlaceModifierToolBuilder
{
	GENERATED_BODY()

public:
	virtual const FToolTargetTypeRequirements& GetTargetRequirements() const override;
};

#endif // WITH_EDITORONLY_DATA
