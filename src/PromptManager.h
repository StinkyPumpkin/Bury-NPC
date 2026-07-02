#pragma once

namespace PromptManager
{
	void Init();
	void Shutdown();

	void ShowForRef(RE::TESObjectREFR* a_ref);
	void ShowGraveForRef(RE::TESObjectREFR* a_ref);
	void Hide();

	void RefreshHotkeys();
}
