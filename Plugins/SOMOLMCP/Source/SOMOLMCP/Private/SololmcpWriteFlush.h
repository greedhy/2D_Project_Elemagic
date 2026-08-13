// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once
#include "RenderingThread.h"
#include "UObject/Package.h"
#include "UObject/Object.h"

namespace SololmcpWriteFlush {
    /** Call after any compile/recompile/asset mutation to ensure subsequent reads see fresh state.
     *  Flushes pending render commands and marks the asset's package dirty. */
    inline void EnsureFlushed(UObject* AssetMaybe) {
        FlushRenderingCommands();
        if (AssetMaybe) {
            if (UPackage* Pkg = AssetMaybe->GetOutermost()) {
                Pkg->MarkPackageDirty();
            }
        }
    }
}
