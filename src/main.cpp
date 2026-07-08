#include "pch.h"
#include "Settings.h"
#include "PromptManager.h"
#include "RespectManager.h"
#include "PickupManager.h"

#include <ShlObj.h>       // SHGetKnownFolderPath (log dir resolve — 1170 gotcha)
#include <KnownFolders.h>
#include <filesystem>

namespace
{
	// SKSE::log::log_directory() returns a bogus path on the 1.6.1170 runtime
	// (…\Skyrim.INI\SKSE\), so no log file appears. Resolve Documents ourselves.
	std::filesystem::path ResolveLogDirectory()
	{
		wchar_t* docs = nullptr;
		std::filesystem::path p;
		if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs))) {
			p = docs;
			::CoTaskMemFree(docs);
		} else if (const wchar_t* up = _wgetenv(L"USERPROFILE")) {
			p = std::filesystem::path(up) / "Documents";
		}
		p /= "My Games";
		p /= "Skyrim Special Edition";
		p /= "SKSE";
		return p;
	}

	void InitLogger()
	{
		auto path = ResolveLogDirectory();
		std::error_code ec;
		std::filesystem::create_directories(path, ec);
		path /= std::format("{}.log", Plugin::NAME);
		try {
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
			auto log = std::make_shared<spdlog::logger>(Plugin::NAME, std::move(sink));
			log->set_level(spdlog::level::info);
			log->flush_on(spdlog::level::info);
			spdlog::set_default_logger(std::move(log));
			spdlog::set_pattern("[%H:%M:%S] [%l] %v");
		} catch (...) {}
	}

	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			PFR::Settings::GetSingleton().Load();
			RespectManager::OnDataLoaded();
			PickupManager::Init();
			PromptManager::Init();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogger();
	SKSE::Init(a_skse);

	logger::info("{} v{} loaded", Plugin::NAME, Plugin::VERSION);

	// Cosave callbacks must be registered at load, before any save is read.
	PickupManager::RegisterSerialization();

	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
	return true;
}
