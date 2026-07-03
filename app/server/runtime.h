#pragma once

#include "config.h"
#include "http.h"

#include "engine/framework/io/json.h"
#include "engine/framework/runtime/model.h"
#include "engine/framework/runtime/session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace minitts::server {

class ServerState final : public IHttpHandler {
public:
    ServerState(ServerConfig config, std::filesystem::path request_base);
    ~ServerState() override;

    HttpResponse handle(const HttpRequest & request) override;

private:
    struct LoadedModel {
        ServerModelConfig config;
        engine::runtime::TaskSpec task;
        std::unique_ptr<engine::runtime::ILoadedVoiceModel> model;
        std::unique_ptr<engine::runtime::IVoiceTaskSession> session;
        engine::runtime::IOfflineVoiceTaskSession * offline = nullptr;
        std::mutex mutex;
        std::chrono::steady_clock::time_point last_used{};  // guarded by mutex
        int idle_timeout_s = 0;                              // effective (resolved) idle TTL
        std::atomic<bool> loaded{false};                     // lock-free status readout
    };

    void load_models();
    void ensure_model_loaded_locked(LoadedModel & model);
    void unload_locked(LoadedModel & model);  // caller holds model.mutex
    LoadedModel & require_model(const engine::io::json::Value & body);
    struct TimedTaskResult;
    TimedTaskResult run_model(LoadedModel & model, const engine::runtime::TaskRequest & request);
    HttpResponse handle_speech(const std::string & body_text);
    HttpResponse handle_transcription(const std::string & body_text);
    HttpResponse handle_generic_run(const std::string & body_text);
    HttpResponse handle_model_lifecycle(const HttpRequest & request);
    std::string models_json() const;

    void start_reaper();
    void reaper_loop();

    ServerConfig config_;
    std::filesystem::path request_base_;
    std::vector<std::unique_ptr<LoadedModel>> models_;
    std::unordered_map<std::string, size_t> model_index_;

    std::thread reaper_thread_;
    std::mutex reaper_mutex_;
    std::condition_variable reaper_cv_;
    bool reaper_stop_ = false;
};

}  // namespace minitts::server
