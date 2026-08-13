#include "Protocol/SololmcpRouter.h"
#include "Tools/SololmcpToolRegistry.h"
#include "Transport/SololmcpHttpTransport.h"
#include "Transport/SololmcpTcpTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSololmcpStandardPingTest,
	"SOMOL.MCP.Session.StandardPing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSololmcpStandardPingTest::RunTest(const FString& Parameters)
{
	using namespace UE::SOMOLMCP;
	FSololmcpToolRegistry Registry;
	FSololmcpRouter Router(Registry);
	const FString Response = Router.HandleMessage(
		42,
		TEXT("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\",\"params\":{}}"));

	TSharedPtr<FJsonObject> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response);
	TestTrue(TEXT("ping response is valid JSON"), FJsonSerializer::Deserialize(Reader, Parsed));
	TestTrue(TEXT("ping response has a result"), Parsed.IsValid() && Parsed->HasTypedField<EJson::Object>(TEXT("result")));
	TestEqual(TEXT("ping preserves request id"), Parsed.IsValid() ? Parsed->GetIntegerField(TEXT("id")) : -1, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSololmcpSessionCapacityDefaultsTest,
	"SOMOL.MCP.Session.CapacityDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSololmcpSessionCapacityDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace UE::SOMOLMCP;
	const FSololmcpHttpTransport::FConfig HttpConfig;
	const FSololmcpTcpTransport::FSecurityConfig TcpConfig;
	TestEqual(TEXT("HTTP default session capacity"), HttpConfig.MaxSessions, 8192);
	TestEqual(TEXT("TCP default connection capacity"), TcpConfig.MaxConnections, 8192);
	TestTrue(TEXT("idle session cleanup is enabled"), HttpConfig.IdleSessionTimeoutSeconds > 0);
	TestTrue(TEXT("expired-session tombstones are bounded"), HttpConfig.MaxExpiredSessionTombstones > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
