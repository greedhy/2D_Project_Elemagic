// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

namespace UE::SOMOLMCP
{
	class FSololmcpToolRegistry;

	/**
	 * Registers the cross-domain authoring QA/delivery gates and the remaining
	 * high-value native PCG graph replacements. This unit is intentionally
	 * standalone so the module owner can wire it into the registry separately.
	 */
	void RegisterAuthoringQaPcgCompletionTools(FSololmcpToolRegistry& Registry);
}
