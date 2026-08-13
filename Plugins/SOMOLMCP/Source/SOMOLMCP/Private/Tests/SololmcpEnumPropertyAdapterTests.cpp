#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Services/SololmcpEditorServices.h"

using namespace UE::SOMOLMCP;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSololmcpByteEnumStringAdapterTest,
    "SOMOL.MCP.EditorServices.ByteEnumStringAdapter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSololmcpByteEnumStringAdapterTest::RunTest(const FString&)
{
    USceneCaptureComponent2D* Component = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
    if (!TestNotNull(TEXT("Transient reflected byte-enum target"), Component)) return false;

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("CaptureSource"), TEXT("SCS_FinalColorLDR"));
    TArray<TSharedPtr<FJsonValue>> Receipts;
    FString Error;
    const FSololmcpEditorServices Services;
    const bool bApplied = Services.ApplyPropertiesWithReceipts(Component, Properties, Receipts, Error);
    TestTrue(*FString::Printf(TEXT("String byte enum applies: %s"), *Error), bApplied);
    TestEqual(TEXT("String resolved to the non-zero enum value"),
        static_cast<int32>(Component->CaptureSource.GetValue()),
        static_cast<int32>(SCS_FinalColorLDR));
    TestEqual(TEXT("One verified property receipt"), Receipts.Num(), 1);

    TSharedRef<FJsonObject> Invalid = MakeShared<FJsonObject>();
    Invalid->SetStringField(TEXT("CaptureSource"), TEXT("NotARealCaptureSource"));
    Receipts.Reset();
    Error.Reset();
    TestFalse(TEXT("Unknown byte enum fails closed"),
        Services.ApplyPropertiesWithReceipts(Component, Invalid, Receipts, Error));
    TestTrue(TEXT("Unknown byte enum produces a diagnostic"), Error.Contains(TEXT("Invalid enum value")));
    return true;
}

#endif
