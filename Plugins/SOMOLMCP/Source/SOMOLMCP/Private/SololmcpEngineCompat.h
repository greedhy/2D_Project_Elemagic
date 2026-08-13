// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

// Engine header path compatibility across UE 5.3 - 5.8.
//
// Some engine headers moved between versions with no overlap: the old path is gone
// by 5.8 and the new path does not exist before 5.5, so **no single include line
// compiles on all six supported engines**. Including the wrong one is a fatal
// C1083 that stops compilation of the whole translation unit — which is how a
// single stale include used to cost UE 5.3 an entire tool family.
//
// Measured availability (scan of Engine/Source + Engine/Plugins per install):
//
//   UserDefinedStruct.h
//     Engine/UserDefinedStruct.h        5.3 5.4 5.5 5.6 5.7  -
//     StructUtils/UserDefinedStruct.h    -   -  5.5 5.6 5.7 5.8
//
//   InstancedStruct.h
//     InstancedStruct.h                 5.3 5.4 5.5 5.6 5.7  -
//     StructUtils/InstancedStruct.h      -   -  5.5 5.6 5.7 5.8
//
//   PropertyBag.h
//     PropertyBag.h                     5.3 5.4 5.5 5.6 5.7  -
//     StructUtils/PropertyBag.h          -   -  5.5 5.6 5.7 5.8
//
//   PerPlatformProperties.h
//     PerPlatformProperties.h           5.3 5.4 5.5 5.6 5.7 5.8   <- unprefixed path
//     UObject/PerPlatformProperties.h    -   -  5.5 5.6 5.7 5.8      works everywhere
//
// The 5.5 boundary is where both spellings coexist, so that is the switch point.
// PerPlatformProperties needs no switch at all: include it unprefixed and it
// resolves on every engine — the UObject/ spelling is the one that breaks 5.3/5.4.
//
// Add a header here rather than gating call sites whenever the *only* difference
// is where a declaration lives. Real API differences (changed signatures, missing
// symbols, absent export macros) belong in per-tool L2 gates instead.

#include "Runtime/Launch/Resources/Version.h"

/**
 * True when the Landscape Edit Layer object API is available (UE 5.6+):
 * ULandscapeEditLayerBase, ALandscape::GetEditLayers/GetEditLayer, and the
 * GetGuid/IsVisible/IsLocked/GetAlphaForTargetType accessors on a layer.
 * Before 5.6 landscapes expose edit layers as FLandscapeLayer values instead, with
 * no equivalent object API, so dependent tools are compiled out rather than shimmed.
 */
#define SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS \
	(ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6))

/**
 * True when World Partition exposes actor descriptors as *instances* (UE 5.4+):
 * FWorldPartitionActorDescInstance, IWorldPartitionActorDescInstanceView, and
 * FWorldPartitionHelpers::ForEachActorDescInstance.
 *
 * UE 5.3 has the pre-instance shape (FWorldPartitionActorDesc /
 * FWorldPartitionActorDescView / ForEachActorDesc), and its streaming-generation
 * error handler is a genuinely different interface rather than the same one with
 * renamed types: 5.3 requires OnInvalidReferenceLevelScriptStreamed and
 * OnInvalidReferenceLevelScriptDataLayers, drops the EDataLayerInvalidReason and
 * EDataLayerHierarchyInvalidReason parameters, and has no HLOD-layer or
 * world-reference callbacks at all. That is why the collector is written twice
 * instead of being typedef'd across.
 */
#define SOMOLMCP_HAS_ACTORDESC_INSTANCE \
	(ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))

// Descriptor type and enumeration helper under whichever name this engine uses.
// Only the spelling differs for these two; the callback interface does not, which
// is why the error collector still needs two separate implementations.
#if SOMOLMCP_HAS_ACTORDESC_INSTANCE
#define SOMOLMCP_ACTOR_DESC          FWorldPartitionActorDescInstance
#define SOMOLMCP_FOREACH_ACTOR_DESC  FWorldPartitionHelpers::ForEachActorDescInstance
#else
#define SOMOLMCP_ACTOR_DESC          FWorldPartitionActorDesc
#define SOMOLMCP_FOREACH_ACTOR_DESC  FWorldPartitionHelpers::ForEachActorDesc
#endif

/** True when the StructUtils/ prefixed spellings are available (UE 5.5+). */
#define SOMOLMCP_STRUCTUTILS_PREFIXED_HEADERS \
	(ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5))

// ── opt-in include shims ───────────────────────────────────────────────────
//
// The macros above are free to include anywhere. The header shims are NOT:
// pulling four engine headers into every file that only wants a version macro
// pushed SololmcpDomainTools.cpp (45k lines) past MSVC's limit and produced a
// C1001 internal compiler error on UE 5.7. Ask for a shim explicitly:
//
//     #define SOMOLMCP_COMPAT_NEED_STRUCTUTILS
//     #define SOMOLMCP_COMPAT_NEED_PERPLATFORM
//     #include "SololmcpEngineCompat.h"

#ifdef SOMOLMCP_COMPAT_NEED_STRUCTUTILS
#if SOMOLMCP_STRUCTUTILS_PREFIXED_HEADERS
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "StructUtils/UserDefinedStruct.h"
#else
#include "InstancedStruct.h"
#include "PropertyBag.h"
#include "Engine/UserDefinedStruct.h"
#endif
#endif // SOMOLMCP_COMPAT_NEED_STRUCTUTILS

#ifdef SOMOLMCP_COMPAT_NEED_PERPLATFORM
// Both spellings resolve on 5.5+, but the unprefixed one is deprecated there and
// warns (C4996); it is the only one that exists on 5.3/5.4.
#if SOMOLMCP_STRUCTUTILS_PREFIXED_HEADERS
#include "UObject/PerPlatformProperties.h"
#else
#include "PerPlatformProperties.h"
#endif
#endif // SOMOLMCP_COMPAT_NEED_PERPLATFORM
