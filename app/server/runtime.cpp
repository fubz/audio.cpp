#include "runtime.h"

#include "../cli/request.h"

#include "engine/framework/io/json.h"
#include "engine/framework/runtime/registry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace minitts::server {
namespace {

using engine::io::json::Value;

using Clock = std::chrono::steady_clock;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string json_quote(std::string_view value) {
    return engine::io::json::stringify_string(value);
}

std::filesystem::path resolve_path(const std::filesystem::path & base, const std::filesystem::path & path) {
    return path.is_absolute() ? path : base / path;
}

std::unordered_map<std::string, std::string> options_from_object(const Value * value) {
    return minitts::cli::json_options_map(value);
}

void add_option_from_json(
    std::unordered_map<std::string, std::string> & options,
    const Value & object,
    const std::string & field,
    const std::string & option_key) {
    const auto * value = object.find(field);
    if (value != nullptr && !value->is_null()) {
        options[option_key] = minitts::cli::json_option_string(*value);
    }
}

std::vector<uint8_t> encode_pcm16_wav(const engine::runtime::AudioBuffer & audio) {
    if (audio.sample_rate <= 0) {
        throw std::runtime_error("audio output sample rate must be positive");
    }
    if (audio.channels <= 0) {
        throw std::runtime_error("audio output channel count must be positive");
    }
    if (audio.samples.size() % static_cast<size_t>(audio.channels) != 0) {
        throw std::runtime_error("audio output sample count must be divisible by channel count");
    }

    const uint16_t channels = static_cast<uint16_t>(audio.channels);
    const uint16_t bits_per_sample = 16;
    const uint32_t data_bytes = static_cast<uint32_t>(audio.samples.size() * sizeof(int16_t));
    const uint32_t riff_size = 36 + data_bytes;
    const uint32_t byte_rate = static_cast<uint32_t>(audio.sample_rate) * channels * bits_per_sample / 8;
    const uint16_t block_align = channels * bits_per_sample / 8;

    std::vector<uint8_t> out;
    out.reserve(44 + data_bytes);
    auto append_bytes = [&](const void * data, size_t size) {
        const auto * bytes = static_cast<const uint8_t *>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    auto append_u16 = [&](uint16_t value) { append_bytes(&value, sizeof(value)); };
    auto append_u32 = [&](uint32_t value) { append_bytes(&value, sizeof(value)); };

    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    append_u32(riff_size);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    append_u32(16);
    append_u16(1);
    append_u16(channels);
    append_u32(static_cast<uint32_t>(audio.sample_rate));
    append_u32(byte_rate);
    append_u16(block_align);
    append_u16(bits_per_sample);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    append_u32(data_bytes);
    for (float sample : audio.samples) {
        sample = std::max(-1.0F, std::min(1.0F, sample));
        const auto pcm = static_cast<int16_t>(std::lrint(sample * 32767.0F));
        append_bytes(&pcm, sizeof(pcm));
    }
    return out;
}

std::string base64_encode(const uint8_t * data, size_t size) {
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
        const uint32_t chunk = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? kAlphabet[(chunk >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < size ? kAlphabet[chunk & 0x3f] : '=');
    }
    return out;
}

std::string base64_encode(const std::vector<uint8_t> & bytes) {
    return base64_encode(bytes.data(), bytes.size());
}

std::vector<uint8_t> base64_decode(const std::string & in) {
    auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;  // skip whitespace / newlines / other
    };
    std::vector<uint8_t> out;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = sextet(c);
        if (v < 0) continue;
        buffer = (buffer << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

// Voice names become on-disk directory names — keep them filesystem-safe.
bool is_valid_voice_name(const std::string & name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

double elapsed_ms(Clock::time_point started) {
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

double audio_duration_ms(const engine::runtime::AudioBuffer & audio) {
    if (audio.sample_rate <= 0 || audio.channels <= 0) {
        return 0.0;
    }
    return 1000.0 * static_cast<double>(audio.samples.size()) /
        static_cast<double>(audio.sample_rate * audio.channels);
}

double audio_rtf(double wall_ms, double duration_ms) {
    return duration_ms > 0.0 ? wall_ms / duration_ms : 0.0;
}

std::string timing_json(double wall_ms) {
    std::ostringstream out;
    out << "{\"wall_ms\":" << wall_ms << "}";
    return out.str();
}

std::string timing_json(double wall_ms, const engine::runtime::AudioBuffer & audio) {
    const double duration_ms = audio_duration_ms(audio);
    std::ostringstream out;
    out << "{\"wall_ms\":" << wall_ms
        << ",\"audio_duration_ms\":" << duration_ms
        << ",\"rtf\":" << audio_rtf(wall_ms, duration_ms) << "}";
    return out.str();
}

std::unordered_map<std::string, std::string> timing_headers(
    double wall_ms,
    const engine::runtime::AudioBuffer & audio) {
    const double duration_ms = audio_duration_ms(audio);
    return {
        {"X-AudioCPP-Wall-Ms", std::to_string(wall_ms)},
        {"X-AudioCPP-Audio-Duration-Ms", std::to_string(duration_ms)},
        {"X-AudioCPP-RTF", std::to_string(audio_rtf(wall_ms, duration_ms))},
    };
}

std::string task_result_json(const engine::runtime::TaskResult & result, double wall_ms) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    auto field = [&](const std::string & name) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << json_quote(name) << ":";
    };

    if (result.text_output.has_value()) {
        field("text");
        out << json_quote(result.text_output->text);
        if (!result.text_output->language.empty()) {
            field("language");
            out << json_quote(result.text_output->language);
        }
    }
    if (result.audio_output.has_value()) {
        const auto wav = encode_pcm16_wav(*result.audio_output);
        field("audio");
        out << json_quote(base64_encode(wav));
        field("sample_rate");
        out << result.audio_output->sample_rate;
        field("channels");
        out << result.audio_output->channels;
    }
    if (!result.named_audio_outputs.empty()) {
        field("named_audio_outputs");
        out << "[";
        for (size_t i = 0; i < result.named_audio_outputs.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            const auto wav = encode_pcm16_wav(result.named_audio_outputs[i].audio);
            out << "{\"id\":" << json_quote(result.named_audio_outputs[i].id)
                << ",\"audio\":" << json_quote(base64_encode(wav))
                << ",\"sample_rate\":" << result.named_audio_outputs[i].audio.sample_rate
                << ",\"channels\":" << result.named_audio_outputs[i].audio.channels
                << "}";
        }
        out << "]";
    }
    if (!result.speech_segments.empty()) {
        field("segments");
        out << "[";
        for (size_t i = 0; i < result.speech_segments.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            const auto & segment = result.speech_segments[i];
            out << "{\"start_sample\":" << segment.span.start_sample
                << ",\"end_sample\":" << segment.span.end_sample
                << ",\"confidence\":" << segment.confidence << "}";
        }
        out << "]";
    }
    if (!result.speaker_turns.empty()) {
        field("speaker_turns");
        out << "[";
        for (size_t i = 0; i < result.speaker_turns.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            const auto & turn = result.speaker_turns[i];
            out << "{\"start_sample\":" << turn.span.start_sample
                << ",\"end_sample\":" << turn.span.end_sample
                << ",\"speaker_id\":" << json_quote(turn.speaker_id)
                << ",\"confidence\":" << turn.confidence << "}";
        }
        out << "]";
    }
    if (!result.word_timestamps.empty()) {
        field("words");
        out << "[";
        for (size_t i = 0; i < result.word_timestamps.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            const auto & word = result.word_timestamps[i];
            out << "{\"word\":" << json_quote(word.word)
                << ",\"start_sample\":" << word.span.start_sample
                << ",\"end_sample\":" << word.span.end_sample
                << ",\"confidence\":" << word.confidence << "}";
        }
        out << "]";
    }
    field("timing");
    if (result.audio_output.has_value()) {
        out << timing_json(wall_ms, *result.audio_output);
    } else if (result.named_audio_outputs.size() == 1) {
        out << timing_json(wall_ms, result.named_audio_outputs.front().audio);
    } else {
        out << timing_json(wall_ms);
    }
    out << "}";
    return out.str();
}

const engine::runtime::AudioBuffer & select_audio_output(const engine::runtime::TaskResult & result) {
    if (result.audio_output.has_value()) {
        return *result.audio_output;
    }
    if (result.named_audio_outputs.size() == 1) {
        return result.named_audio_outputs.front().audio;
    }
    throw std::runtime_error("model result did not contain exactly one audio output");
}

engine::runtime::TaskRequest build_openai_speech_request(const Value & body, const std::filesystem::path & base_dir) {
    engine::runtime::TaskRequest request;
    request.text_input = engine::runtime::Transcript{
        engine::io::json::require_string(body, "input"),
        engine::io::json::optional_string(body, "language", ""),
    };

    engine::runtime::VoiceCondition voice;
    bool has_voice = false;
    if (const auto * value = body.find("voice")) {
        engine::runtime::VoiceReference reference;
        reference.cached_voice_id = value->as_string();
        voice.speaker = std::move(reference);
        has_voice = true;
    }
    if (const auto * value = body.find("voice_ref")) {
        if (!voice.speaker.has_value()) {
            voice.speaker = engine::runtime::VoiceReference{};
        }
        voice.speaker->audio = minitts::cli::read_audio_buffer(resolve_path(base_dir, value->as_string()));
        has_voice = true;
    }
    if (has_voice) {
        request.voice = std::move(voice);
    }

    request.options = options_from_object(body.find("options"));
    add_option_from_json(request.options, body, "seed", "seed");
    add_option_from_json(request.options, body, "temperature", "temperature");
    add_option_from_json(request.options, body, "top_k", "top_k");
    add_option_from_json(request.options, body, "top_p", "top_p");
    add_option_from_json(request.options, body, "max_tokens", "max_tokens");
    add_option_from_json(request.options, body, "max_steps", "max_steps");
    add_option_from_json(request.options, body, "repetition_penalty", "repetition_penalty");
    add_option_from_json(request.options, body, "guidance_scale", "guidance_scale");
    add_option_from_json(request.options, body, "num_inference_steps", "num_inference_steps");
    if (const auto * value = body.find("instructions")) {
        request.options["instruct"] = value->as_string();
    }
    if (const auto * value = body.find("reference_text")) {
        request.options["reference_text"] = value->as_string();
    }
    return request;
}

engine::runtime::TaskRequest build_openai_transcription_request(const Value & body, const std::filesystem::path & base_dir) {
    engine::runtime::TaskRequest request;

    // Inline audio (base64 WAV) — for callers that can't drop a server-local file
    // (e.g. a remote client / another container). Decoded to a temp file so the
    // existing WAV reader can consume it.
    if (const auto * b64 = body.find("audio_b64"); b64 != nullptr && b64->is_string()) {
        static std::atomic<uint64_t> counter{0};
        const auto bytes = base64_decode(b64->as_string());
        const auto tmp = std::filesystem::temp_directory_path() /
            ("audiocpp-stt-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".wav");
        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        request.audio_input = minitts::cli::read_audio_buffer(tmp);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    } else {
        const auto * audio = body.find("audio");
        if (audio == nullptr) {
            audio = body.find("audio_path");
        }
        if (audio == nullptr) {
            audio = body.find("file");
        }
        if (audio == nullptr || !audio->is_string()) {
            throw std::runtime_error("transcription request requires audio_b64, audio, audio_path, or file");
        }
        request.audio_input = minitts::cli::read_audio_buffer(resolve_path(base_dir, audio->as_string()));
    }

    request.options = options_from_object(body.find("options"));
    if (const auto * value = body.find("language")) {
        request.options["language"] = value->as_string();
    }
    return request;
}

}  // namespace

ServerState::ServerState(ServerConfig config, std::filesystem::path request_base)
    : config_(std::move(config)),
      request_base_(std::move(request_base)),
      voices_dir_(request_base_ / "voices") {
    last_inference_ms_.store(now_ms(), std::memory_order_relaxed);
    load_models();
    start_reaper();
}

ServerState::~ServerState() {
    {
        std::lock_guard<std::mutex> lock(reaper_mutex_);
        reaper_stop_ = true;
    }
    reaper_cv_.notify_all();
    if (reaper_thread_.joinable()) {
        reaper_thread_.join();
    }
}

HttpResponse ServerState::handle(const HttpRequest & request) {
    if (request.method == "GET" && request.path == "/health") {
        return json_response("{\"status\":\"ok\",\"backend\":\"cuda\",\"models\":" + std::to_string(models_.size()) + "}");
    }
    if (request.method == "GET" && request.path == "/v1/models") {
        return json_response(models_json());
    }
    if (request.method == "POST" && request.path == "/v1/audio/speech") {
        return handle_speech(request.body);
    }
    if (request.method == "POST" && request.path == "/v1/audio/transcriptions") {
        return handle_transcription(request.body);
    }
    if (request.method == "POST" && request.path == "/v1/tasks/run") {
        return handle_generic_run(request.body);
    }
    if (request.method == "POST" && request.path.rfind("/v1/models/", 0) == 0) {
        return handle_model_lifecycle(request);
    }
    if (request.path == "/v1/voices" || request.path.rfind("/v1/voices/", 0) == 0) {
        return handle_voices(request);
    }
    return error_response(404, "unknown endpoint: " + request.path, "not_found");
}

void ServerState::load_models() {
    for (auto & config : config_.models) {
        auto loaded = std::make_unique<LoadedModel>();
        loaded->config = std::move(config);
        loaded->task = engine::runtime::TaskSpec{
            engine::runtime::parse_voice_task_kind(loaded->config.task),
            engine::runtime::parse_run_mode(loaded->config.mode),
        };
        if (loaded->task.mode != engine::runtime::RunMode::Offline) {
            throw std::runtime_error("audiocpp_server currently requires offline model sessions");
        }
        if (!model_index_.emplace(loaded->config.id, models_.size()).second) {
            throw std::runtime_error("duplicate server model id: " + loaded->config.id);
        }
        loaded->idle_timeout_s = loaded->config.idle_timeout_s >= 0
            ? loaded->config.idle_timeout_s
            : config_.idle_timeout_s;
        if (!loaded->config.lazy) {
            ensure_model_loaded_locked(*loaded);
        }
        models_.push_back(std::move(loaded));
    }
}

void ServerState::ensure_model_loaded_locked(LoadedModel & model) {
    if (model.session != nullptr) {
        return;
    }
    // Make room first: on a small/shared GPU two large models can't co-reside, so
    // free the least-recently-used resident model(s) before this cudaMalloc runs.
    evict_for_load_locked(model);
    auto registry = engine::runtime::make_default_registry();

    engine::runtime::ModelLoadRequest load_request;
    load_request.model_path = model.config.path;
    load_request.family_hint = model.config.family;
    load_request.config_id = model.config.config_id;
    load_request.weight_id = model.config.weight_id;
    load_request.options = model.config.load_options;

    engine::runtime::SessionOptions session_options;
    session_options.backend.type = engine::core::BackendType::Cuda;
    session_options.backend.device = config_.device;
    session_options.backend.threads = config_.threads;
    session_options.options = model.config.session_options;

    auto loaded_model = registry.load(load_request);
    auto session = loaded_model->create_task_session(model.task, session_options);
    auto * offline = dynamic_cast<engine::runtime::IOfflineVoiceTaskSession *>(session.get());
    if (offline == nullptr) {
        throw std::runtime_error("configured model does not provide offline execution: " + model.config.id);
    }
    model.model = std::move(loaded_model);
    model.session = std::move(session);
    model.offline = offline;
    model.last_used = Clock::now();
    model.last_used_ms_.store(now_ms(), std::memory_order_relaxed);
    model.loaded.store(true, std::memory_order_relaxed);
    ever_loaded_.store(true, std::memory_order_relaxed);
    last_inference_ms_.store(now_ms(), std::memory_order_relaxed);
}

void ServerState::evict_for_load_locked(LoadedModel & loading) {
    if (config_.max_resident_models <= 0) {
        return;  // unlimited residency (default) — nothing to enforce
    }
    std::lock_guard<std::mutex> guard(residency_mutex_);

    // Currently-resident models other than the one we're about to load.
    std::vector<LoadedModel *> resident;
    for (auto & entry : models_) {
        if (entry.get() != &loading && entry->loaded.load(std::memory_order_relaxed)) {
            resident.push_back(entry.get());
        }
    }
    // Evict enough to leave a free slot for `loading` (which is not yet resident).
    int to_evict = static_cast<int>(resident.size()) - config_.max_resident_models + 1;
    if (to_evict <= 0) {
        return;
    }
    // Least-recently-used first (lock-free read of the LRU mirror).
    std::sort(resident.begin(), resident.end(), [](const LoadedModel * a, const LoadedModel * b) {
        return a->last_used_ms_.load(std::memory_order_relaxed) <
               b->last_used_ms_.load(std::memory_order_relaxed);
    });
    for (LoadedModel * victim : resident) {
        if (to_evict <= 0) {
            break;
        }
        // try_lock, never block: a model with an in-flight request holds its mutex and
        // can't be evicted (and blocking here could deadlock against another loader).
        std::unique_lock<std::mutex> victim_lock(victim->mutex, std::try_to_lock);
        if (!victim_lock.owns_lock() || victim->session == nullptr) {
            continue;  // busy, or already unloaded since we snapshotted
        }
        unload_locked(*victim);
        std::cout << "audiocpp_server: evicted model \"" << victim->config.id
                  << "\" to load \"" << loading.config.id
                  << "\" (max_resident_models=" << config_.max_resident_models << ")\n";
        --to_evict;
    }
}

void ServerState::unload_locked(LoadedModel & model) {
    if (model.session == nullptr) {
        return;
    }
    // Destruction order matters: the session owns the ggml compute graph/backend
    // buffers, the loaded model owns the weight buffers. Both free their CUDA
    // allocations in their destructors, returning VRAM to the device.
    model.offline = nullptr;
    model.session.reset();
    model.model.reset();
    model.loaded.store(false, std::memory_order_relaxed);
}

ServerState::LoadedModel & ServerState::require_model_by_id(const std::string & id) {
    const auto it = model_index_.find(id);
    if (it == model_index_.end()) {
        throw std::runtime_error("unknown model id: " + id);
    }
    return *models_.at(it->second);
}

ServerState::LoadedModel & ServerState::require_model(const Value & body) {
    return require_model_by_id(engine::io::json::require_string(body, "model"));
}

struct ServerState::TimedTaskResult {
    engine::runtime::TaskResult result;
    double wall_ms = 0.0;
};

ServerState::TimedTaskResult ServerState::run_model(
    LoadedModel & model,
    const engine::runtime::TaskRequest & request) {
    std::lock_guard<std::mutex> lock(model.mutex);
    ensure_model_loaded_locked(model);
    const auto started = Clock::now();
    model.session->prepare(engine::runtime::build_preparation_request(request));
    auto result = model.offline->run(request);
    model.last_used = Clock::now();
    model.last_used_ms_.store(now_ms(), std::memory_order_relaxed);
    last_inference_ms_.store(now_ms(), std::memory_order_relaxed);
    return TimedTaskResult{std::move(result), elapsed_ms(started)};
}

HttpResponse ServerState::handle_speech(const std::string & body_text) {
    const auto body = engine::io::json::parse(body_text);

    // If "voice" names a registered voice, it carries its own definition (and
    // possibly its own model). Otherwise "voice" falls through to the model's
    // native cached-voice handling (e.g. Qwen3 CustomVoice packaged speakers).
    std::optional<VoiceManifest> voice;
    if (const auto * value = body.find("voice"); value != nullptr && value->is_string()) {
        voice = lookup_voice(value->as_string());
    }
    std::string model_id;
    if (const auto * value = body.find("model"); value != nullptr && value->is_string()) {
        model_id = value->as_string();
    } else if (voice.has_value() && !voice->model.empty()) {
        model_id = voice->model;
    } else {
        throw std::runtime_error(
            "speech request requires \"model\" (or a registered \"voice\" that carries one)");
    }
    auto & model = require_model_by_id(model_id);

    auto request = build_openai_speech_request(body, request_base_);
    if (voice.has_value()) {
        if (voice->mode == "clone") {
            engine::runtime::VoiceReference reference;
            reference.audio = minitts::cli::read_audio_buffer(voice->ref_wav);
            engine::runtime::VoiceCondition condition;
            condition.speaker = std::move(reference);
            request.voice = std::move(condition);
            if (!voice->reference_text.empty()) {
                request.options["reference_text"] = voice->reference_text;
            }
        } else if (voice->mode == "design") {
            request.voice.reset();  // designed voices carry no speaker reference
            request.options["instruct"] = voice->instruct;
        }
    }
    const auto timed_result = run_model(model, request);
    const auto & audio = select_audio_output(timed_result.result);
    const auto wav = encode_pcm16_wav(audio);
    const auto response_format = engine::io::json::optional_string(body, "response_format", "wav");
    if (response_format == "json" || response_format == "b64_json") {
        return json_response(
            "{\"audio\":" + json_quote(base64_encode(wav)) +
            ",\"format\":\"wav\",\"timing\":" + timing_json(timed_result.wall_ms, audio) + "}");
    }
    return HttpResponse{
        200,
        "audio/wav",
        std::string(reinterpret_cast<const char *>(wav.data()), wav.size()),
        timing_headers(timed_result.wall_ms, audio),
    };
}

HttpResponse ServerState::handle_transcription(const std::string & body_text) {
    const auto body = engine::io::json::parse(body_text);
    auto & model = require_model(body);
    const auto request = build_openai_transcription_request(body, request_base_);
    const auto timed_result = run_model(model, request);
    const auto & result = timed_result.result;
    if (!result.text_output.has_value()) {
        throw std::runtime_error("model result did not contain transcript text");
    }
    return json_response(
        "{\"text\":" + json_quote(result.text_output->text) +
        ",\"timing\":" + timing_json(timed_result.wall_ms) + "}");
}

HttpResponse ServerState::handle_generic_run(const std::string & body_text) {
    const auto body = engine::io::json::parse(body_text);
    auto & model = require_model(body);
    const auto * request_json = body.find("request");
    const auto request = minitts::cli::build_request_from_json(
        request_json != nullptr ? *request_json : body,
        request_base_);
    const auto timed_result = run_model(model, request);
    return json_response(task_result_json(timed_result.result, timed_result.wall_ms));
}

std::string ServerState::models_json() const {
    std::ostringstream out;
    out << "{\"object\":\"list\",\"data\":[";
    for (size_t i = 0; i < models_.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        const auto & model = *models_[i];
        out << "{\"id\":" << json_quote(model.config.id)
            << ",\"object\":\"model\""
            << ",\"owned_by\":\"engine\""
            << ",\"family\":" << json_quote(model.config.family)
            << ",\"task\":" << json_quote(engine::runtime::to_string(model.task.task))
            << ",\"mode\":" << json_quote(engine::runtime::to_string(model.task.mode))
            << ",\"loaded\":" << (model.loaded.load(std::memory_order_relaxed) ? "true" : "false")
            << ",\"idle_timeout_s\":" << model.idle_timeout_s
            << "}";
    }
    out << "]}";
    return out.str();
}

HttpResponse ServerState::handle_model_lifecycle(const HttpRequest & request) {
    // Routes: POST /v1/models/{id}/load  and  POST /v1/models/{id}/unload
    static const std::string prefix = "/v1/models/";
    const std::string rest = request.path.substr(prefix.size());
    const auto slash = rest.rfind('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
        return error_response(404, "unknown endpoint: " + request.path, "not_found");
    }
    const std::string id = rest.substr(0, slash);      // model ids may contain '/'
    const std::string action = rest.substr(slash + 1);
    const auto it = model_index_.find(id);
    if (it == model_index_.end()) {
        return error_response(404, "unknown model id: " + id, "not_found");
    }
    LoadedModel & model = *models_.at(it->second);

    if (action == "unload") {
        std::lock_guard<std::mutex> lock(model.mutex);
        const bool was_loaded = model.session != nullptr;
        unload_locked(model);
        return json_response(
            "{\"id\":" + json_quote(id) +
            ",\"unloaded\":" + (was_loaded ? "true" : "false") +
            ",\"loaded\":false}");
    }
    if (action == "load") {
        std::lock_guard<std::mutex> lock(model.mutex);
        ensure_model_loaded_locked(model);
        return json_response("{\"id\":" + json_quote(id) + ",\"loaded\":true}");
    }
    return error_response(404, "unknown model action: " + action, "not_found");
}

void ServerState::start_reaper() {
    const bool any_idle = std::any_of(models_.begin(), models_.end(), [](const auto & m) {
        return m->idle_timeout_s > 0;
    });
    if (!any_idle && config_.idle_exit_after_s <= 0) {
        return;  // nothing opts into idle unload or idle exit; skip the background thread
    }
    reaper_thread_ = std::thread([this] { reaper_loop(); });
}

void ServerState::reaper_loop() {
    const auto interval = std::chrono::seconds(config_.reaper_interval_s);
    std::unique_lock<std::mutex> lock(reaper_mutex_);
    while (!reaper_stop_) {
        reaper_cv_.wait_for(lock, interval, [this] { return reaper_stop_; });
        if (reaper_stop_) {
            break;
        }
        lock.unlock();
        const auto now = Clock::now();
        for (auto & entry : models_) {
            LoadedModel & model = *entry;
            std::lock_guard<std::mutex> model_lock(model.mutex);
            if (model.session == nullptr || model.idle_timeout_s <= 0) {
                continue;
            }
            if (now - model.last_used >= std::chrono::seconds(model.idle_timeout_s)) {
                unload_locked(model);
                std::cout << "audiocpp_server: idle-unloaded model \"" << model.config.id
                          << "\" after " << model.idle_timeout_s << "s\n";
            }
        }
        // Idle-exit: once every model has unloaded and the server has been idle past the
        // grace period, exit so the CUDA primary context is released (ggml can't hot-release
        // it — only process exit does). The restart policy brings us back cold at ~P8 idle.
        if (config_.idle_exit_after_s > 0 && ever_loaded_.load(std::memory_order_relaxed)) {
            const bool any_loaded = std::any_of(models_.begin(), models_.end(),
                [](const auto & m) { return m->loaded.load(std::memory_order_relaxed); });
            const std::int64_t idle_ms = now_ms() - last_inference_ms_.load(std::memory_order_relaxed);
            if (!any_loaded && idle_ms >= static_cast<std::int64_t>(config_.idle_exit_after_s) * 1000) {
                std::cout << "audiocpp_server: idle " << (idle_ms / 1000)
                          << "s with no models loaded — exiting to release the GPU"
                             " (restart policy brings it back cold)." << std::endl;
                std::_Exit(0);
            }
        }
        lock.lock();
    }
}

std::string ServerState::voice_to_json(const VoiceManifest & voice) const {
    std::ostringstream out;
    out << "{\"name\":" << json_quote(voice.name)
        << ",\"mode\":" << json_quote(voice.mode)
        << ",\"model\":" << json_quote(voice.model)
        << ",\"reference_text\":" << json_quote(voice.reference_text)
        << ",\"instruct\":" << json_quote(voice.instruct)
        << "}";
    return out.str();
}

std::optional<ServerState::VoiceManifest> ServerState::lookup_voice(const std::string & name) const {
    if (!is_valid_voice_name(name)) {
        return std::nullopt;
    }
    const auto manifest_path = voices_dir_ / name / "voice.json";
    std::error_code ec;
    if (!std::filesystem::exists(manifest_path, ec)) {
        return std::nullopt;
    }
    const auto root = engine::io::json::parse_file(manifest_path);
    VoiceManifest voice;
    voice.name = name;
    voice.mode = engine::io::json::optional_string(root, "mode", "clone");
    voice.model = engine::io::json::optional_string(root, "model", "");
    voice.reference_text = engine::io::json::optional_string(root, "reference_text", "");
    voice.instruct = engine::io::json::optional_string(root, "instruct", "");
    voice.ref_wav = voices_dir_ / name / "ref.wav";
    return voice;
}

HttpResponse ServerState::handle_voices(const HttpRequest & request) {
    if (request.path == "/v1/voices") {
        if (request.method == "GET") {
            return list_voices();
        }
        if (request.method == "POST") {
            return create_voice(request.body);
        }
        return error_response(405, "method not allowed: " + request.method, "method_not_allowed");
    }
    const std::string prefix = "/v1/voices/";
    const std::string name = request.path.substr(prefix.size());
    if (!is_valid_voice_name(name)) {
        return error_response(404, "unknown endpoint: " + request.path, "not_found");
    }
    if (request.method == "GET") {
        const auto voice = lookup_voice(name);
        if (!voice.has_value()) {
            return error_response(404, "unknown voice: " + name, "not_found");
        }
        return json_response(voice_to_json(*voice));
    }
    if (request.method == "DELETE") {
        return delete_voice(name);
    }
    return error_response(405, "method not allowed: " + request.method, "method_not_allowed");
}

HttpResponse ServerState::create_voice(const std::string & body_text) {
    const auto body = engine::io::json::parse(body_text);
    const std::string name = engine::io::json::require_string(body, "name");
    if (!is_valid_voice_name(name)) {
        return error_response(400, "voice name must match [A-Za-z0-9_-] and be 1..64 chars", "invalid_request");
    }

    VoiceManifest voice;
    voice.name = name;
    voice.model = engine::io::json::optional_string(body, "model", "");
    voice.reference_text = engine::io::json::optional_string(body, "reference_text", "");
    voice.instruct = engine::io::json::optional_string(body, "instruct", "");

    // Sample source for a clone voice: inline base64 or a server-local path.
    std::vector<uint8_t> sample;
    bool has_sample = false;
    if (const auto * value = body.find("sample_b64"); value != nullptr && value->is_string()) {
        sample = base64_decode(value->as_string());
        has_sample = true;
    } else if (const auto * value = body.find("sample_path"); value != nullptr && value->is_string()) {
        const auto path = resolve_path(request_base_, value->as_string());
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return error_response(400, "cannot read sample_path: " + path.string(), "invalid_request");
        }
        sample.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        has_sample = true;
    }

    if (has_sample) {
        voice.mode = "clone";
    } else if (!voice.instruct.empty()) {
        voice.mode = "design";
    } else {
        return error_response(
            400, "voice requires sample_b64/sample_path (clone) or instruct (design)", "invalid_request");
    }

    const auto dir = voices_dir_ / name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return error_response(500, "failed to create voice directory: " + ec.message(), "server_error");
    }
    if (voice.mode == "clone") {
        std::ofstream out(dir / "ref.wav", std::ios::binary);
        out.write(reinterpret_cast<const char *>(sample.data()), static_cast<std::streamsize>(sample.size()));
        if (!out) {
            return error_response(500, "failed to write voice sample", "server_error");
        }
        voice.ref_wav = dir / "ref.wav";
    }
    std::ofstream manifest(dir / "voice.json");
    manifest << voice_to_json(voice);
    if (!manifest) {
        return error_response(500, "failed to write voice manifest", "server_error");
    }
    return json_response(voice_to_json(voice), 201);
}

HttpResponse ServerState::list_voices() const {
    std::ostringstream out;
    out << "{\"object\":\"list\",\"data\":[";
    std::error_code ec;
    bool first = true;
    if (std::filesystem::exists(voices_dir_, ec)) {
        for (const auto & entry : std::filesystem::directory_iterator(voices_dir_, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            const auto voice = lookup_voice(entry.path().filename().string());
            if (!voice.has_value()) {
                continue;
            }
            if (!first) {
                out << ",";
            }
            first = false;
            out << voice_to_json(*voice);
        }
    }
    out << "]}";
    return json_response(out.str());
}

HttpResponse ServerState::delete_voice(const std::string & name) {
    const auto dir = voices_dir_ / name;
    std::error_code ec;
    const bool existed = std::filesystem::exists(dir, ec);
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        return error_response(500, "failed to delete voice: " + ec.message(), "server_error");
    }
    return json_response(
        "{\"name\":" + json_quote(name) + ",\"deleted\":" + (existed ? "true" : "false") + "}");
}

}  // namespace minitts::server
