// Fill out your copyright notice in the Description page of Project Settings.

#include "AttackFrameData.h"
#include "PaperFlipbook.h"

float UAttackFrameData::GetTotalDuration() const
{
    if (!SourceAnimation.IsNull())
    {
        if (UPaperFlipbook* Flipbook = SourceAnimation.LoadSynchronous())
        {
            return Flipbook->GetTotalDuration();
        }
    }

    // Fallback: 最后一帧 NormalizedTime 反算,默认 0.5s 总时长
    if (Frames.Num() > 0)
    {
        const float LastTime = Frames.Last().NormalizedTime;
        if (LastTime > 0.f)
        {
            return 0.5f / LastTime;
        }
    }
    return 0.5f;
}
