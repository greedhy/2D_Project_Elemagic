# 单向平台(One-Way Platform) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a reusable `AOneWayPlatform` C++ base Actor that characters can jump through from below and stand on from above, verified end-to-end with a placeholder Blueprint in `level1`.

**Architecture:** A single C++ Actor (`UBoxComponent` collision + `UPaperSpriteComponent` visual) that extracts its core "should this character pass through right now" decision into a pure, `UWorld`-independent static function (same pattern as `ACharacterBase::SelectFlipbookForState` from the movement-control plan), then uses that decision each Tick to add/remove itself from each overlapping character's `UCapsuleComponent::MoveIgnoreActors` — the standard UE technique for one-way platforms, since it lets `UCharacterMovementComponent` keep doing real physical collision/support rather than faking it with manual Z-snapping.

**Tech Stack:** UE 5.8, C++ (Paper2D `UPaperSpriteComponent`, `UBoxComponent`, `ACharacter`/`UCapsuleComponent`), Blueprint (`BP_OneWayPlatform_Placeholder`), UE Automation Testing framework (`Misc/AutomationTest.h`).

## Global Constraints

- Per `docs/superpowers/specs/2026-08-08-oneway-platform-design.md`: **all future one-way platforms must inherit from `AOneWayPlatform`** — don't create a parallel implementation later; new platform types are BP subclasses that only swap the sprite.
- Per `docs/superpowers/specs/2026-08-07-elemagic-framework-design.md`: new Actor base classes live under `Source/Elemagic/{Public,Private}/Actor/`; C++ owns mechanism, Blueprint owns content (sprite/visuals).
- This project has no automated test infra beyond what the character-movement-control plan introduced. Write a UE Automation Test **only** for the pure `ShouldPassThroughPlatform` function (no `UWorld` needed). Everything that needs a live character physically moving through the platform is a manual PIE checklist, not an invented automated harness — matches how this project (and its reference, test_25d) is actually validated.
- Comments in this codebase are written in Chinese at decision points — match that style for any new non-obvious comment.
- Compile with (PowerShell, not Bash, so the Windows path resolves correctly): `& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" ElemagicEditor Win64 Development -Project="C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -WaitMutex`
  **Before running this, check whether `UnrealEditor.exe` is running** (`tasklist //FI "IMAGENAME eq UnrealEditor.exe"` in Bash). If it is, Live Coding locks the DLL and `Build.bat` fails with "Unable to build while Live Coding is active" — ask the user to close the editor first (or use Ctrl+Alt+F11 Live Coding compile instead, for .cpp-only changes with no new UPROPERTY/UFUNCTION).
- Run automation tests headlessly with: `& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -ExecCmds="Automation RunTests <TestName>;Quit" -unattended -nopause -nullrhi -log=AutomationTest.log` then check `Saved\Logs\Elemagic.log` (or `Elemagic_2.log` if another editor instance is running and already owns `Elemagic.log`) for `Result={Success}` next to the test name.
- Every task ends with a commit, per `feedback_dev_approach` in project memory: one verified feature/slice at a time.

## File Structure

- `Source/Elemagic/Public/Actor/OneWayPlatform.h` (new) — `AOneWayPlatform` class declaration: components, the pure `ShouldPassThroughPlatform` function, Tick/overlap-handler declarations.
- `Source/Elemagic/Private/Actor/OneWayPlatform.cpp` (new) — implementation.
- `Source/Elemagic/Private/Tests/OneWayPlatformTests.cpp` (new) — automation test for `ShouldPassThroughPlatform`.
- `Content/Blueprint/Actor/BP_OneWayPlatform_Placeholder.uasset` (new, created in-editor in Task 2) — placeholder BP subclass with a simple sprite assigned.
- `Content/Level/level1.umap` (modified in-editor in Task 2) — one placed instance of the BP for verification.

---

### Task 1: `AOneWayPlatform` scaffold + testable pass-through decision

**Files:**
- Create: `Source/Elemagic/Public/Actor/OneWayPlatform.h`
- Create: `Source/Elemagic/Private/Actor/OneWayPlatform.cpp`
- Create: `Source/Elemagic/Private/Tests/OneWayPlatformTests.cpp`
- Test: `Source/Elemagic/Private/Tests/OneWayPlatformTests.cpp`

**Interfaces:**
- Produces: `static bool AOneWayPlatform::ShouldPassThroughPlatform(float CharacterFeetZ, float PlatformTopZ, float CharacterVelocityZ)` — pure function, no side effects, no `UWorld` required. Rule: if `CharacterFeetZ >= PlatformTopZ` the character is already at/above the platform → never pass through (must be supported, regardless of velocity). Otherwise (character is below), pass through only while rising (`CharacterVelocityZ > 0.f`).
- Produces: `AOneWayPlatform` class with `CollisionBox` (`UBoxComponent`, root) and `Sprite` (`UPaperSpriteComponent`, attached to root) components, both `protected`.
- Consumes: nothing (first task).

- [ ] **Step 1: Write the failing test**

Create `Source/Elemagic/Private/Tests/OneWayPlatformTests.cpp`:

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Actor/OneWayPlatform.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOneWayPlatformPassThroughTest, "Elemagic.OneWayPlatform.ShouldPassThroughPlatform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOneWayPlatformPassThroughTest::RunTest(const FString& Parameters)
{
	const float PlatformTopZ = 100.f;

	TestTrue(TEXT("Below platform and rising should pass through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, 200.f));

	TestFalse(TEXT("Below platform and falling should not pass through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, -200.f));

	TestFalse(TEXT("Below platform and stationary should not pass through"),
		AOneWayPlatform::ShouldPassThroughPlatform(50.f, PlatformTopZ, 0.f));

	TestFalse(TEXT("Exactly at platform top while rising should not pass through (already arrived)"),
		AOneWayPlatform::ShouldPassThroughPlatform(100.f, PlatformTopZ, 200.f));

	TestFalse(TEXT("Above platform and falling onto it should not pass through (gets supported)"),
		AOneWayPlatform::ShouldPassThroughPlatform(150.f, PlatformTopZ, -200.f));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
```

- [ ] **Step 2: Run the build to verify it fails**

Check first whether the editor is running (`tasklist //FI "IMAGENAME eq UnrealEditor.exe"` in Bash); if so, ask the user to close it. Then run (PowerShell):
```
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" ElemagicEditor Win64 Development -Project="C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -WaitMutex
```
Expected: FAILS — `Actor/OneWayPlatform.h`: No such file or directory (the header referenced by the test doesn't exist yet). This is the "red" step; a compiled language fails to build instead of failing at runtime.

- [ ] **Step 3: Implement the minimal code to make it pass**

Create `Source/Elemagic/Public/Actor/OneWayPlatform.h`:

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OneWayPlatform.generated.h"

class UBoxComponent;
class UPaperSpriteComponent;
class ACharacter;
class UPrimitiveComponent;

/**
 * 单向平台基类:玩家(及以后的敌人)可以从下方跳穿,落到上方后能被正常支撑站立。
 * 具体外观(木板、石台等)都是本类的蓝图子类,只换 Sprite 贴图,不动这里的逻辑——
 * 以后所有单向平台都应该继承这个类,不要另开平行实现。
 */
UCLASS()
class ELEMAGIC_API AOneWayPlatform : public AActor
{
	GENERATED_BODY()

public:
	AOneWayPlatform();

	virtual void Tick(float DeltaTime) override;

	// 纯函数,不依赖 UWorld,方便单独做自动化测试。
	// 角色脚底已到达/高于平台顶面时(不论速度方向)永远不穿透,保证落地后能被稳定支撑;
	// 只有在平台下方且正在上升时才允许穿透。
	static bool ShouldPassThroughPlatform(float CharacterFeetZ, float PlatformTopZ, float CharacterVelocityZ);

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UBoxComponent> CollisionBox;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Elemagic|Platform")
	TObjectPtr<UPaperSpriteComponent> Sprite;

	UFUNCTION()
	void OnPlatformEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

private:
	float GetPlatformTopZ() const;
};
```

Create `Source/Elemagic/Private/Actor/OneWayPlatform.cpp`:

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "Actor/OneWayPlatform.h"
#include "Components/BoxComponent.h"
#include "PaperSpriteComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CapsuleComponent.h"

AOneWayPlatform::AOneWayPlatform()
{
	PrimaryActorTick.bCanEverTick = true;

	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	RootComponent = CollisionBox;
	CollisionBox->SetBoxExtent(FVector(50.f, 50.f, 10.f));
	CollisionBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	CollisionBox->SetGenerateOverlapEvents(true);

	Sprite = CreateDefaultSubobject<UPaperSpriteComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AOneWayPlatform::BeginPlay()
{
	Super::BeginPlay();

	CollisionBox->OnComponentEndOverlap.AddDynamic(this, &AOneWayPlatform::OnPlatformEndOverlap);
}

bool AOneWayPlatform::ShouldPassThroughPlatform(float CharacterFeetZ, float PlatformTopZ, float CharacterVelocityZ)
{
	if (CharacterFeetZ >= PlatformTopZ)
	{
		return false;
	}
	return CharacterVelocityZ > 0.f;
}

float AOneWayPlatform::GetPlatformTopZ() const
{
	return CollisionBox->GetComponentLocation().Z + CollisionBox->GetScaledBoxExtent().Z;
}

void AOneWayPlatform::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// Task 2 补上重叠角色的 MoveIgnoreActors 切换逻辑。
}

void AOneWayPlatform::OnPlatformEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	// Task 2 补上离开重叠范围时的清理逻辑。
}
```

- [ ] **Step 4: Run the build and the test to verify it passes**

Run the same `Build.bat` command as Step 2. Expected: builds with no errors.

Then run:
```
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -ExecCmds="Automation RunTests Elemagic.OneWayPlatform.ShouldPassThroughPlatform;Quit" -unattended -nopause -nullrhi -log=AutomationTest.log
```
Check `Saved\Logs\Elemagic.log` (or `Elemagic_2.log` if the interactive editor is also running) for a line containing `Elemagic.OneWayPlatform.ShouldPassThroughPlatform` and `Result={Success}`.

- [ ] **Step 5: Commit**

```bash
git add Source/Elemagic/Public/Actor/OneWayPlatform.h Source/Elemagic/Private/Actor/OneWayPlatform.cpp Source/Elemagic/Private/Tests/OneWayPlatformTests.cpp
git commit -m "feat: add AOneWayPlatform scaffold with testable pass-through rule"
```

---

### Task 2: Wire MoveIgnoreActors toggling, add a placeholder BP, and verify in PIE

**Files:**
- Modify: `Source/Elemagic/Private/Actor/OneWayPlatform.cpp`
- Create (in-editor): `Content/Blueprint/Actor/BP_OneWayPlatform_Placeholder.uasset`
- Modify (in-editor): `Content/Level/level1.umap`

**Interfaces:**
- Consumes: `AOneWayPlatform::ShouldPassThroughPlatform` and the `CollisionBox`/`Sprite` components from Task 1.
- Produces: a working, PIE-verified one-way platform — nothing further in this plan depends on this task's output; it's the end-to-end verification gate for the spec's "单向平台" feature.

This task has no new automated test — the underlying decision logic is already covered by Task 1's test, and what's left (does `MoveIgnoreActors` actually make `UCharacterMovementComponent` behave correctly, does a Blueprint-placed instance work in a real level) can only be checked by playing it, matching this project's established testing approach.

- [ ] **Step 1: Implement the Tick and end-overlap wiring**

In `Source/Elemagic/Private/Actor/OneWayPlatform.cpp`, replace the `Tick` and `OnPlatformEndOverlap` bodies:

```cpp
void AOneWayPlatform::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	TArray<AActor*> OverlappingActors;
	CollisionBox->GetOverlappingActors(OverlappingActors, ACharacter::StaticClass());

	const float PlatformTopZ = GetPlatformTopZ();

	for (AActor* OverlappingActor : OverlappingActors)
	{
		ACharacter* Character = Cast<ACharacter>(OverlappingActor);
		UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
		if (!Capsule)
		{
			continue;
		}

		const float FeetZ = Capsule->GetComponentLocation().Z - Capsule->GetScaledCapsuleHalfHeight();
		const float VelocityZ = Character->GetVelocity().Z;

		if (ShouldPassThroughPlatform(FeetZ, PlatformTopZ, VelocityZ))
		{
			Capsule->MoveIgnoreActors.AddUnique(this);
		}
		else
		{
			Capsule->MoveIgnoreActors.Remove(this);
		}
	}
}

void AOneWayPlatform::OnPlatformEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	// 保险丝:角色离开重叠范围时,不管 Tick 当时把它设成了穿透还是阻挡,
	// 都强制把这块平台从它的 MoveIgnoreActors 里摘掉,避免角色绕开平台后
	// 这块平台被永久标记为"忽略"、之后再也挡不住它。
	if (ACharacter* Character = Cast<ACharacter>(OtherActor))
	{
		if (UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
		{
			Capsule->MoveIgnoreActors.Remove(this);
		}
	}
}
```

- [ ] **Step 2: Build**

Check first whether the editor is running (as in Task 1 Step 2); close it if so. Run the same `Build.bat` command. Expected: builds with no errors.

- [ ] **Step 3: Re-run Task 1's automation test to confirm no regression**

Run the same `UnrealEditor-Cmd.exe` command from Task 1 Step 4. Expected: still `Result={Success}` — the Tick/overlap changes didn't touch `ShouldPassThroughPlatform` itself.

- [ ] **Step 4: Create the placeholder Blueprint and place it in level1**

Open the editor (`Elemagic.uproject`). In the Content Browser, create folder `Content/Blueprint/Actor/` if it doesn't exist. Right-click → Blueprint Class → search for `OneWayPlatform` as the parent class → name it `BP_OneWayPlatform_Placeholder`. Open it, select the `Sprite` component, and assign any existing flat/rectangular sprite as a stand-in visual (e.g. one of the `Tilemap_Platform`-derived sprites in `Content/Asset/`, or leave the default checkerboard if none fits — this is explicitly a placeholder per the spec, swappable later without code changes). Compile and Save.

Open `Content/Level/level1`, drag one instance of `BP_OneWayPlatform_Placeholder` into the level, positioned a bit above the existing ground tiles with clear space underneath for the character to jump up into it from below.

- [ ] **Step 5: Manual PIE verification checklist**

Press Play in `level1` and verify each of the following, in order:

1. Standing under the platform and jumping straight up: the character passes through the bottom of the platform on the way up (no collision block on the way up).
2. After passing through, the character's upward velocity runs out and they fall back down — they should land **on top of** the platform and stay there (supported, not falling through).
3. While standing on top, walking normally works (no jitter, no sinking into the platform).
4. Walking off the edge of the platform: the character falls normally off the side, no getting stuck.
5. Repeat steps 1–4 several times in a row (jump through, land, walk off, jump through again) — confirm no permanent stuck-in-ignore state (this is what Task 2 Step 1's `OnPlatformEndOverlap` safety net is meant to prevent).
6. Check the Output Log for errors — there should be none related to `OneWayPlatform`.

If any check fails, fix the underlying cause and repeat the checklist from the top.

- [ ] **Step 6: Commit**

```bash
git status
git add Source/Elemagic/Private/Actor/OneWayPlatform.cpp Content/Blueprint/Actor/BP_OneWayPlatform_Placeholder.uasset Content/Level/level1.umap
git commit -m "feat: wire one-way platform pass-through/support behavior and verify in level1"
```

(Only `git add` the specific files `git status` shows as new/modified — don't blanket-add.)

---

## Self-Review Notes

- **Spec coverage:** covers the full `AOneWayPlatform` design from `docs/superpowers/specs/2026-08-08-oneway-platform-design.md` — component structure (Task 1), pure pass-through function + its automated test (Task 1), `MoveIgnoreActors` wiring (Task 2), and all 4 points of the spec's manual verification checklist (Task 2 Step 5, expanded to 6 points to also cover the repeat-cycle and Output Log checks the spec's "反复跳上跳下多次没有卡死或抖动" and "没有报错" bullets call for). The spec's "未决问题" (moving one-way platforms, multi-character edge cases) are explicitly out of scope for this plan, matching the spec.
- **Placeholder scan:** no TBD/TODO left. The BP sprite assignment in Task 2 Step 4 is intentionally a placeholder per the spec, not an unfinished plan step — the spec explicitly calls for a placeholder-first approach here, same as `IdleFlipbook`/`RunFlipbook` were handled in the movement-control plan.
- **Type consistency:** `ShouldPassThroughPlatform`'s signature (Task 1) is used identically in `Tick()` (Task 2) and in the test file. `CollisionBox`/`Sprite`/`GetPlatformTopZ()` names are consistent across both files.
