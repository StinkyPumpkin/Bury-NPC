#include "pch.h"
#include "Settings.h"
#include "PromptManager.h"
#include "RespectManager.h"
#include "PickupManager.h"

namespace
{
	void InitLogger()
	{
		auto path = SKSE::log::log_directory();
		if (!path) return;
		*path /= std::format("{}.log", Plugin::NAME);
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>(Plugin::NAME, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S] [%l] %v");
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
