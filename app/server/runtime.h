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

    // A named, server-stored voice. "clone" carries a reference sample (+ its
    // transcript); "design" carries a text instruction. `model` is the server
    // model id this voice synthesizes against (may be empty → caller supplies).
    struct VoiceManifest {
        std::string name;
        std::string mode;            // "clone" | "design"
        std::string model;
        std::string reference_text;  // clone
        std::string instruct;        // design
        std::filesystem::path ref_wav;
    };

    void load_models();
    void ensure_model_loaded_locked(LoadedModel & model);
    void unload_locked(LoadedModel & model);  // caller holds model.mutex
    LoadedModel & require_model(const engine::io::json::Value & body);
    LoadedModel & require_model_by_id(const std::string & id);
    struct TimedTaskResult;
    TimedTaskResult run_model(LoadedModel & model, const engine::runtime::TaskRequest & request);
    HttpResponse handle_speech(const std::string & body_text);
    HttpResponse handle_transcription(const std::string & body_text);
    HttpResponse handle_generic_run(const std::string & body_text);
    HttpResponse handle_model_lifecycle(const HttpRequest & request);
    std::string models_json() const;

    // Voice registry (POST/GET/DELETE /v1/voices[/{name}]).
    HttpResponse handle_voices(const HttpRequest & request);
    HttpResponse create_voice(const std::string & body_text);
    HttpResponse list_voices() const;
    HttpResponse delete_voice(const std::string & name);
    std::optional<VoiceManifest> lookup_voice(const std::string & name) const;
    std::string voice_to_json(const VoiceManifest & voice) const;

    void start_reaper();
    void reaper_loop();

    ServerConfig config_;
    std::filesystem::path request_base_;
    std::filesystem::path voices_dir_;
    std::vector<std::unique_ptr<LoadedModel>> models_;
    std::unordered_map<std::string, size_t> model_index_;

    std::thread reaper_thread_;
    std::mutex reaper_mutex_;
    std::condition_variable reaper_cv_;
    bool reaper_stop_ = false;
};

}  // namespace minitts::server
