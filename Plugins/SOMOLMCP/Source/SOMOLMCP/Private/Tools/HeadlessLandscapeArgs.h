// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

// Define a safe struct for passing heightmap arguments directly to the C++ Landscape Generator
struct FHeadlessLandscapeArgs {
    int32 Width = 0;
    int32 Height = 0;
    int32 NumSubsections = 1;
    int32 SubsectionSizeQuads = 63;
    int32 QuadsPerComponent = 63;
    int32 ComponentCountX = 1;
    int32 ComponentCountY = 1;
    FVector Location = FVector::ZeroVector;
    FVector Scale = FVector(100.0f, 100.0f, 100.0f);
    TArray<uint16> HeightData;

    bool IsImportResolutionConsistent() const
    {
        const int32 SectionQuads = SubsectionSizeQuads > 0 ? SubsectionSizeQuads : QuadsPerComponent;
        const int64 ExpectedWidth = static_cast<int64>(ComponentCountX) * FMath::Max(1, NumSubsections) * SectionQuads + 1;
        const int64 ExpectedHeight = static_cast<int64>(ComponentCountY) * FMath::Max(1, NumSubsections) * SectionQuads + 1;
        const int64 ExpectedSamples = ExpectedWidth * ExpectedHeight;
        return Width == ExpectedWidth && Height == ExpectedHeight && ExpectedSamples > 0 &&
            ExpectedSamples <= MAX_int32 && HeightData.Num() == ExpectedSamples;
    }
};
