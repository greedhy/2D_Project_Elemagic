# 角色移动控制(Character Movement Control) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the already-partially-wired run/jump/facing-flip movement on `APlayerCharacter` into a verified, good-feeling first playable slice — including a falling/jump animation state that the current Idle/Run-only flipbook switch doesn't have — so it can serve as the foundation the rest of Elemagic's features build on.

**Architecture:** No new systems are introduced. This plan extends the existing `ACharacterBase`/`APlayerCharacter` (Paper2D `APaperCharacter` + `UCharacterMovementComponent`) and `AMyPlayerController`/Enhanced-Input pipeline that framework exploration found already in place: `Move()`/`Jump()`/`StopJumping()` are already bound, `JumpMaxHoldTime` already gives variable jump height, and `UpdateAnimation()`/`UpdateFacing()` already exist but only distinguish Idle vs Run. This plan (1) extracts the flipbook-selection logic into a pure, unit-testable function and adds a falling/jump state to it, (2) tunes `UCharacterMovementComponent` constants for a platformer feel instead of the engine's 3D-game defaults, and (3) verifies the whole chain end-to-end in PIE, including the Blueprint asset wiring (Input assets, flipbooks, GameMode classes, level geometry) that can't be checked from C++ alone.

**Tech Stack:** UE 5.8, C++ (Paper2D, Enhanced Input, GameplayAbilities), Blueprint (`BP_PlayerCharacter`, `BP_MyPlayerController`, `BP_GameMode`), UE Automation Testing framework (`Misc/AutomationTest.h`, part of `Core` — no `Elemagic.Build.cs` changes needed).

## Global Constraints

- Comments in this codebase are written in Chinese at decision points (see existing `CharacterBase.h`/`.cpp`) — match that style for any new non-obvious comment.
- C++ owns mechanism; content (specific flipbook assets, exact Input asset bindings, level geometry) is configured in Blueprint/editor, not hardcoded — per `docs/superpowers/specs/2026-08-07-elemagic-framework-design.md`.
- Neither Elemagic nor its reference project test_25d has any existing automated test infrastructure — this project is normally verified by playing it in PIE (Play-In-Editor). This plan introduces automation tests **only** where logic can be tested without a `UWorld` (pure functions, CDO property inspection); everything else (input wiring, animation actually playing, jump feel) is verified with an explicit, repeatable manual PIE checklist instead of an invented automated harness that wouldn't match how this project is actually validated.
- Compile with: `& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" ElemagicEditor Win64 Development -Project="C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -WaitMutex` (run via the PowerShell tool, not Bash, so the Windows path resolves correctly).
- Run automation tests headlessly with: `& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -ExecCmds="Automation RunTests <TestName>;Quit" -unattended -nopause -nullrhi -log=AutomationTest.log` then check `Saved\Logs\AutomationTest.log` for `Result={Passed}` next to the test name.
- Every task ends with a commit, per `feedback_dev_approach` in project memory: one verified feature/slice at a time.

## File Structure

- `Source/Elemagic/Public/CharacterBase.h` — add `JumpFlipbook` property and the new static `SelectFlipbookForState` helper's declaration.
- `Source/Elemagic/Private/CharacterBase.cpp` — implement `SelectFlipbookForState`; make `UpdateAnimation()` falling-aware.
- `Source/Elemagic/Private/PlayerCharacter.cpp` — tune `UCharacterMovementComponent` constants for platformer feel in the constructor.
- `Source/Elemagic/Private/Tests/CharacterAnimationTests.cpp` (new) — automation test for `SelectFlipbookForState`.
- `Source/Elemagic/Private/Tests/PlayerCharacterMovementTests.cpp` (new) — automation test for the tuned movement constants.
- `Content/Blueprint/BP_MyPlayerController.uasset`, `Content/Blueprint/BP_PlayerCharacter.uasset`, `Content/Blueprint/BP_GameMode.uasset`, `Content/Level/level1.umap` — verified/edited in-editor in Task 3 (binary assets, no direct file diff possible).

---

### Task 1: Extract a testable flipbook-selection function and add the falling/jump state

**Files:**
- Modify: `Source/Elemagic/Public/CharacterBase.h`
- Modify: `Source/Elemagic/Private/CharacterBase.cpp`
- Create: `Source/Elemagic/Private/Tests/CharacterAnimationTests.cpp`
- Test: `Source/Elemagic/Private/Tests/CharacterAnimationTests.cpp`

**Interfaces:**
- Produces: `static UPaperFlipbook* ACharacterBase::SelectFlipbookForState(bool bIsFalling, bool bIsMoving, UPaperFlipbook* IdleFlipbook, UPaperFlipbook* RunFlipbook, UPaperFlipbook* JumpFlipbook)` — pure function, no side effects, no `UWorld` required. Priority: falling (if `JumpFlipbook` set) > moving (if `RunFlipbook` set) > idle. Falls back gracefully when a flipbook isn't assigned yet (no jump art exists in `Content/` yet).
- Produces: `TObjectPtr<UPaperFlipbook> ACharacterBase::JumpFlipbook` — new `EditDefaultsOnly, BlueprintReadOnly` property, same pattern as the existing `IdleFlipbook`/`RunFlipbook`.
- Consumes: nothing (first task).

- [ ] **Step 1: Write the failing test**

Create `Source/Elemagic/Private/Tests/CharacterAnimationTests.cpp`:

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CharacterBase.h"
#include "PaperFlipbook.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterBaseSelectFlipbookTest, "Elemagic.CharacterBase.SelectFlipbookForState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterBaseSelectFlipbookTest::RunTest(const FString& Parameters)
{
	UPaperFlipbook* Idle = NewObject<UPaperFlipbook>();
	UPaperFlipbook* Run = NewObject<UPaperFlipbook>();
	UPaperFlipbook* Jump = NewObject<UPaperFlipbook>();

	// UPaperFlipbook* 是裸 UObject 指针,FAutomationTestBase::TestEqual 没有匹配的重载,
	// 用 TestTrue + 手动 == 比较来避免重载决议失败。
	TestTrue(TEXT("Grounded and idle picks Idle flipbook"),
		ACharacterBase::SelectFlipbookForState(false, false, Idle, Run, Jump) == Idle);

	TestTrue(TEXT("Grounded and moving picks Run flipbook"),
		ACharacterBase::SelectFlipbookForState(false, true, Idle, Run, Jump) == Run);

	TestTrue(TEXT("Falling picks Jump flipbook when assigned"),
		ACharacterBase::SelectFlipbookForState(true, true, Idle, Run, Jump) == Jump);

	TestTrue(TEXT("Falling without a Jump flipbook falls back to Run"),
		ACharacterBase::SelectFlipbookForState(true, true, Idle, Run, nullptr) == Run);

	TestTrue(TEXT("Falling without Jump or Run flipbook falls back to Idle"),
		ACharacterBase::SelectFlipbookForState(true, true, Idle, nullptr, nullptr) == Idle);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
```

- [ ] **Step 2: Run the build to verify it fails**

Run (PowerShell):
```
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" ElemagicEditor Win64 Development -Project="C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -WaitMutex
```
Expected: FAILS with an error like `'SelectFlipbookForState': is not a member of 'ACharacterBase'` — the symbol referenced by the test doesn't exist yet. This is the "red" step; a compiled language fails to build instead of failing at runtime.

- [ ] **Step 3: Implement the minimal code to make it pass**

In `Source/Elemagic/Public/CharacterBase.h`, add the new property next to the existing two, and declare the static helper (add to the `public:` section, near `IsDead()`):

```cpp
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Elemagic|Animation")
	TObjectPtr<UPaperFlipbook> JumpFlipbook;

	// 纯函数,不依赖 UWorld,方便单独做自动化测试。
	// 优先级:下落(有 JumpFlipbook 才用)> 移动(有 RunFlipbook 才用)> 待机。
	static UPaperFlipbook* SelectFlipbookForState(bool bIsFalling, bool bIsMoving,
		UPaperFlipbook* IdleFlipbook, UPaperFlipbook* RunFlipbook, UPaperFlipbook* JumpFlipbook);
```

Place `JumpFlipbook` right after the existing `RunFlipbook` property (`Source/Elemagic/Public/CharacterBase.h:41-42`), and the static function declaration after `IsDead()` (`Source/Elemagic/Public/CharacterBase.h:34-35`).

In `Source/Elemagic/Private/CharacterBase.cpp`, add the implementation (e.g. right before `UpdateAnimation`):

```cpp
UPaperFlipbook* ACharacterBase::SelectFlipbookForState(bool bIsFalling, bool bIsMoving,
	UPaperFlipbook* IdleFlipbook, UPaperFlipbook* RunFlipbook, UPaperFlipbook* JumpFlipbook)
{
	if (bIsFalling && JumpFlipbook)
	{
		return JumpFlipbook;
	}
	if (bIsMoving && RunFlipbook)
	{
		return RunFlipbook;
	}
	return IdleFlipbook;
}
```

Do **not** wire this into `UpdateAnimation()` yet — that's Task 2. This task only needs the pure function to exist and compile so the test can pass.

- [ ] **Step 4: Run the build and the test to verify it passes**

Run the same `Build.bat` command as Step 2. Expected: builds with no errors.

Then run:
```
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\greedy\Documents\Unreal Projects\Elemagic\Elemagic.uproject" -ExecCmds="Automation RunTests Elemagic.CharacterBase.SelectFlipbookForState;Quit" -unattended -nopause -nullrhi -log=AutomationTest.log
```
Then check `Saved\Logs\AutomationTest.log` for a line containing `Elemagic.CharacterBase.SelectFlipbookForState` and `Result={Passed}`.

- [ ] **Step 5: Commit**

```bash
git add Source/Elemagic/Public/CharacterBase.h Source/Elemagic/Private/CharacterBase.cpp Source/Elemagic/Private/Tests/CharacterAnimationTests.cpp
git commit -m "feat: add falling-aware flipbook selection for CharacterBase"
```

---

### Task 2: Wire the falling state into UpdateAnimation and tune platformer movement feel

**Files:**
- Modify: `Source/Elemagic/Private/CharacterBase.cpp`
- Modify: `Source/Elemagic/Private/PlayerCharacter.cpp`
- Create: `Source/Elemagic/Private/Tests/PlayerCharacterMovementTests.cpp`
- Test: `Source/Elemagic/Private/Tests/PlayerCharacterMovementTests.cpp`

**Interfaces:**
- Consumes: `ACharacterBase::SelectFlipbookForState` and `ACharacterBase::JumpFlipbook` (Task 1).
- Produces: `UpdateAnimation()` now falling-aware (internal, no new public surface). Tuned `UCharacterMovementComponent` constants on `APlayerCharacter`'s movement component — `MaxWalkSpeed = 600.f`, `JumpZVelocity = 700.f`, `GravityScale = 2.f`, `AirControl = 0.8f`, `BrakingDecelerationWalking = 2048.f` — consumed by Task 3's manual PIE verification.

- [ ] **Step 1: Write the failing test**

Create `Source/Elemagic/Private/Tests/PlayerCharacterMovementTests.cpp`:

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PlayerCharacter.h"
#include "GameFramework/CharacterMovementComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayerCharacterMovementTuningTest, "Elemagic.PlayerCharacter.MovementTuning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayerCharacterMovementTuningTest::RunTest(const FString& Parameters)
{
	const APlayerCharacter* CDO = GetDefault<APlayerCharacter>();
	const UCharacterMovementComponent* MoveComp = CDO ? CDO->GetCharacterMovement() : nullptr;
	if (!TestNotNull(TEXT("APlayerCharacter CDO has a CharacterMovementComponent"), MoveComp))
	{
		return false;
	}

	TestEqual(TEXT("MaxWalkSpeed tuned for a brisk platformer run"), MoveComp->MaxWalkSpeed, 600.f);
	TestEqual(TEXT("JumpZVelocity tuned for a snappy jump"), MoveComp->JumpZVelocity, 700.f);
	TestEqual(TEXT("GravityScale tuned for a tight fall arc"), MoveComp->GravityScale, 2.f);
	TestEqual(TEXT("AirControl tuned for responsive air movement"), MoveComp->AirControl, 0.8f);
	TestEqual(TEXT("BrakingDecelerationWalking tuned to stop crisply"), MoveComp->BrakingDecelerationWalking, 2048.f);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
```

- [ ] **Step 2: Run the build and the test to verify it fails**

Run the `Build.bat` command from Task 1 Step 2 (expected: builds fine, this test doesn't reference any new symbol). Then run the `UnrealEditor-Cmd.exe` command from Task 1 Step 4, replacing the test name with `Elemagic.PlayerCharacter.MovementTuning`.
Expected: FAIL — `APlayerCharacter`'s constructor hasn't set these values yet, so at least the `GravityScale`/`AirControl`/`JumpZVelocity` assertions (which differ sharply from `UCharacterMovementComponent`'s realistic-3D-game defaults) won't match.

- [ ] **Step 3: Write the minimal implementation**

In `Source/Elemagic/Private/PlayerCharacter.cpp`, add the include:

```cpp
#include "GameFramework/CharacterMovementComponent.h"
```

Then, in the constructor, right after the existing `JumpMaxHoldTime = 0.3f;` line (`Source/Elemagic/Private/PlayerCharacter.cpp:26`):

```cpp

	// 横版动作平台跳跃手感:CharacterMovementComponent 默认值是为写实 3D 游戏调的,
	// 这里改成更快的下落速度、更高的空中控制力,让跳跃弧线更"脆"、走位更跟手。
	// 数值先给一版合理起点,具体手感后续可以直接在 BP_PlayerCharacter 的 Class Defaults 里继续微调。
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->MaxWalkSpeed = 600.f;
		MoveComp->JumpZVelocity = 700.f;
		MoveComp->GravityScale = 2.f;
		MoveComp->AirControl = 0.8f;
		MoveComp->BrakingDecelerationWalking = 2048.f;
	}
```

In `Source/Elemagic/Private/CharacterBase.cpp`, replace the body of `UpdateAnimation()` (`Source/Elemagic/Private/CharacterBase.cpp:102-117`) to use the new helper and account for falling:

```cpp
void ACharacterBase::UpdateAnimation()
{
	UPaperFlipbookComponent* SpriteComp = GetSprite();
	if (!SpriteComp)
	{
		return;
	}

	const bool bIsFalling = GetCharacterMovement() && GetCharacterMovement()->IsFalling();
	const bool bIsMoving = !FMath::IsNearlyZero(GetVelocity().X);
	UPaperFlipbook* DesiredFlipbook = SelectFlipbookForState(bIsFalling, bIsMoving, IdleFlipbook, RunFlipbook, JumpFlipbook);

	if (DesiredFlipbook && SpriteComp->GetFlipbook() != DesiredFlipbook)
	{
		SpriteComp->SetFlipbook(DesiredFlipbook);
	}
}
```

- [ ] **Step 4: Run the build and the test to verify it passes**

Same two commands as Step 2. Expected: build succeeds, and the log for `Elemagic.PlayerCharacter.MovementTuning` shows `Result={Passed}`. Also re-run `Elemagic.CharacterBase.SelectFlipbookForState` (Task 1's test) to confirm it's still passing after the `UpdateAnimation()` change.

- [ ] **Step 5: Commit**

```bash
git add Source/Elemagic/Private/CharacterBase.cpp Source/Elemagic/Private/PlayerCharacter.cpp Source/Elemagic/Private/Tests/PlayerCharacterMovementTests.cpp
git commit -m "feat: wire falling animation state and tune platformer movement feel"
```

---

### Task 3: Verify editor wiring and confirm the full movement loop in PIE

**Files:**
- Modify (in-editor, binary assets — no text diff): `Content/Blueprint/BP_MyPlayerController.uasset`, `Content/Blueprint/BP_PlayerCharacter.uasset`, `Content/Blueprint/BP_GameMode.uasset`, `Content/Level/level1.umap` (only if any of the checks below find something unassigned/missing).

**Interfaces:**
- Consumes: Task 1's `JumpFlipbook` property, Task 2's falling-aware `UpdateAnimation()` and tuned movement constants.
- Produces: a confirmed-working movement slice — nothing further in this plan depends on this task's output; it's the end-to-end verification gate for the "角色移动控制" feature described in the spec's roadmap item 1.

This task has no C++ to write. Everything here has to be checked/set in the Unreal Editor because Blueprint asset references live in binary `.uasset` files. Do these checks in order; only touch (and re-save) a Blueprint if a check finds something wrong.

- [ ] **Step 1: Rebuild and open the editor**

Run the `Build.bat` command from Task 1 Step 2 one more time to make sure Tasks 1–2's code is in the binary the editor will load. Then open `Elemagic.uproject` in the Unreal Editor (double-click it, or launch `UnrealEditor.exe` with the `.uproject` path as an argument).

- [ ] **Step 2: Verify BP_MyPlayerController's input assets are assigned**

Open `Content/Blueprint/BP_MyPlayerController`. In the Class Defaults panel, under the "Elemagic|Input" category, confirm:
- `Player Mapping Context` = `IMC_Default`
- `Move Action` = `IA_Move`
- `Jump Action` = `IA_Jump`
- `Input Config` = `DA_EleInputConfig`

If any is empty, assign it to the matching asset in `Content/Blueprint/Input/`. Compile and Save if you changed anything.

- [ ] **Step 3: Verify BP_PlayerCharacter's flipbooks are assigned**

Open `Content/Blueprint/BP_PlayerCharacter`. In the Class Defaults panel, under "Elemagic|Animation", confirm `Idle Flipbook` and `Run Flipbook` are assigned (they should point at the existing `Content/Asset/Player/Player_Idle*` and `Content/Asset/Player_Run` flipbook assets). Leave `Jump Flipbook` empty for now — there's no jump-pose art yet, and Task 1's `SelectFlipbookForState` is written to fall back to `RunFlipbook` when `JumpFlipbook` is unset, so this is expected, not a bug. Compile and Save if you changed anything.

- [ ] **Step 4: Verify BP_GameMode's class references**

Open `Content/Blueprint/BP_GameMode`. Confirm `Default Pawn Class` = `BP_PlayerCharacter` and `Player Controller Class` = `BP_MyPlayerController`. Confirm this GameMode (or one that overrides from it) is set as the project's default GameMode in Project Settings → Maps & Modes.

- [ ] **Step 5: Verify level1 has somewhere to stand and a PlayerStart**

Open `Content/Level/level1`. Confirm there is a `PlayerStart` actor placed, and that there's platform/tile geometry with collision under it (from the existing `Tilemap_Platform` asset) so the character doesn't spawn into a fall. Add a `PlayerStart` if one is missing. Save the map if you changed anything.

- [ ] **Step 6: Restart the editor if this is the first PIE session since EleInputComponent became the default**

`Config/DefaultInput.ini` sets `DefaultInputComponentClass=/Script/Elemagic.EleInputComponent`, but per the comment in `MyPlayerController.cpp:39-43`, this only takes effect after a **full editor restart** (Live Coding recompiles aren't enough). If you haven't restarted the editor since this project was set up, close and reopen it now, before testing.

- [ ] **Step 7: Manual PIE checklist**

Open `level1` and press Play. Verify each of the following, in order:

1. Character spawns standing on the platform (not falling through it, not floating).
2. Pressing the Move-left/right keys (bound via `IA_Move`) translates the character horizontally.
3. The sprite flips to face the direction of movement (`UpdateFacing`).
4. While moving, the flipbook switches from the idle pose to the running pose; releasing the movement key switches it back to idle within a tick.
5. A short tap of the Jump key produces a small hop; holding Jump for close to the full 0.3s (`JumpMaxHoldTime`) produces a noticeably higher jump — confirms the existing variable-height jump still works after the movement tuning.
6. The jump arc feels snappy (fast rise, faster fall) rather than floaty — confirms Task 2's `GravityScale`/`JumpZVelocity` tuning took effect.
7. While airborne, horizontal input still has a visible, responsive effect on trajectory (confirms `AirControl = 0.8`).
8. Character lands cleanly back on the platform and immediately returns to idle/run flipbook state — it doesn't get stuck showing a stale flipbook.
9. Open the Output Log window and confirm there is **no** `SetupInputComponent: InputComponent is not a UEleInputComponent` error. If it's present, you skipped Step 6 — restart the editor and repeat this checklist.

If any check fails, fix the underlying cause (most likely a missing/wrong asset assignment from Steps 2–5, or the Step 6 restart) and repeat the checklist from the top.

- [ ] **Step 8: Commit any Blueprint/level changes made while fixing checks**

Only run this if Steps 2–5 actually changed and saved a Blueprint or the map; otherwise skip (nothing to commit).

```bash
git status
git add Content/Blueprint/BP_MyPlayerController.uasset Content/Blueprint/BP_PlayerCharacter.uasset Content/Blueprint/BP_GameMode.uasset Content/Level/level1.umap
git commit -m "fix: wire missing input/animation/level references for movement control"
```

(Only `git add` the specific files that `git status` actually shows as modified — don't blanket-add.)

---

## Self-Review Notes

- **Spec coverage:** covers roadmap item 1 in full — run (already existed, verified in Task 3), jump incl. variable height (already existed, verified in Task 3), facing flip (already existed, verified in Task 3), and the Idle/Run/Jump flipbook state machine (the actual gap — closed in Tasks 1–2, verified in Task 3). No other spec section applies to this feature.
- **Placeholder scan:** no TBD/TODO left; the one open item (no jump-pose art yet) is handled with an explicit, tested fallback rather than a placeholder comment.
- **Type consistency:** `SelectFlipbookForState`'s signature (Task 1) is used identically in `UpdateAnimation()` (Task 2) and in both test files; `JumpFlipbook` is the same `TObjectPtr<UPaperFlipbook>` type as the existing `IdleFlipbook`/`RunFlipbook` it's declared next to.
