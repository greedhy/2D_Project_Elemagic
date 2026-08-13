// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Tools/SololmcpToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UE::SOMOLMCP
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVideoAutomationRegistryTest,
	"SOMOLMCP.VideoAutomation.RegistryAndClosedSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVideoAutomationRegistryTest::RunTest(const FString&)
{
	FSololmcpToolRegistry Registry;
	const TArray<FString> NewNames = {
		TEXT("desktop_capture_source_list"), TEXT("desktop_capture_session_start"),
		TEXT("desktop_capture_session_status_get"), TEXT("desktop_capture_session_stop"),
		TEXT("desktop_capture_session_cancel"), TEXT("movie_render_graph_capture_preflight"),
		TEXT("sequencer_camera_path_audit"), TEXT("worldforge_runtime_presentation_audit")};
	for (const FString& Name : NewNames)
	{
		TestTrue(*FString::Printf(TEXT("%s is registered"), *Name), Registry.HasRegisteredTool(Name));
	}

	TArray<FString> Names;
	Registry.GetRegisteredToolNamesSorted(Names);
	TSet<FString> Unique(Names);
	TestEqual(TEXT("tool names are unique"), Unique.Num(), Names.Num());

	const TSet<FString> UpgradedNames = {
		TEXT("movie_render_graph_asset_create"), TEXT("movie_render_graph_basic_queue_configure"),
		TEXT("movie_render_graph_compile_validate"), TEXT("movie_render_graph_inspect"),
		TEXT("movie_render_graph_job_submit"), TEXT("movie_render_graph_job_status_get"),
		TEXT("movie_render_graph_job_cancel"), TEXT("movie_render_graph_output_artifact_readback"),
		TEXT("movie_render_graph_visual_qa"), TEXT("video_transcoder_profile_create"),
		TEXT("video_transcoder_job_submit"), TEXT("video_transcoder_job_status_get"),
		TEXT("video_transcoder_job_cancel"), TEXT("video_transcoder_output_validate"), TEXT("video_probe"),
		TEXT("sequencer_event_endpoint_create"), TEXT("sequencer_event_payload_set"),
		TEXT("render_queue_output_guard_plan"), TEXT("mrq_output_validate")};
	for (const FString& Name : UpgradedNames)
	{
		TestTrue(*FString::Printf(TEXT("upgraded executor %s is registered"), *Name), Registry.HasRegisteredTool(Name));
	}
	const TArray<FString> CompatibilityAliases = {
		TEXT("render_queue_submit"), TEXT("render_queue_list"), TEXT("render_queue_cancel"),
		TEXT("mrq_job_create"), TEXT("mrq_job_configure"), TEXT("mrq_queue_list"), TEXT("mrq_render_status"),
		TEXT("sequencer_mrq_job_from_sequence"), TEXT("sequencer_mrq_preset_apply"),
		TEXT("sequencer_mrq_queue_submit"), TEXT("sequencer_mrq_job_poll"), TEXT("sequencer_mrq_job_cancel")};
	for (const FString& Name : CompatibilityAliases)
	{
		TestTrue(*FString::Printf(TEXT("compatibility alias %s is registered"), *Name), Registry.HasRegisteredTool(Name));
	}

	for (const TSharedPtr<FJsonValue>& Value : Registry.BuildToolsList())
	{
		const TSharedPtr<FJsonObject> Tool = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Tool.IsValid()) continue;
		const FString Name = Tool->GetStringField(TEXT("name"));
		if (!NewNames.Contains(Name)) continue;
		const TSharedPtr<FJsonObject>* Schema = nullptr;
		TestTrue(*FString::Printf(TEXT("%s has inputSchema"), *Name), Tool->TryGetObjectField(TEXT("inputSchema"), Schema) && Schema);
		if (Schema && Schema->IsValid())
		{
			bool bAdditional = true;
			TestTrue(*FString::Printf(TEXT("%s declares additionalProperties"), *Name), (*Schema)->TryGetBoolField(TEXT("additionalProperties"), bAdditional));
			TestFalse(*FString::Printf(TEXT("%s schema is closed"), *Name), bAdditional);
		}
	}
	return true;
}
}
#endif
