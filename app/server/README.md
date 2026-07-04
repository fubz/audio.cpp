# audio.cpp Server

`audiocpp_server` is a CUDA-only HTTP adapter over the framework runtime registry. It keeps one loaded model and one offline task session per active model id, so repeated HTTP requests reuse the same framework session and model-owned graph/cache state.

## Build

```bash
cmake -S . -B build -DENGINE_ENABLE_CUDA=ON
cmake --build build --parallel --target audiocpp_server
```

## Config

```bash
cat > server.json <<'JSON'
{
  "host": "127.0.0.1",
  "port": 8080,
  "device": 0,
  "threads": 1,
  "lazy_load": true,
  "models": [
    {
      "id": "pocket-tts",
      "family": "pocket_tts",
      "path": "/path/to/models/pocket-tts",
      "task": "tts",
      "mode": "offline",
      "load_options": {
        "language": "english"
      },
      "session_options": {
        "language": "english"
      }
    },
    {
      "id": "qwen3-asr",
      "family": "qwen3_asr",
      "path": "/path/to/models/Qwen3-ASR-0.6B",
      "task": "asr",
      "mode": "offline"
    }
  ]
}
JSON
```

The server resolves model paths from this JSON exactly as written, so use paths that match your machine. Request-time audio paths are also user-provided paths.

Set top-level `"lazy_load": true` to register all configured model ids at startup but defer each model's framework load and session creation until its first request. A model can override the default with `"lazy": true` or `"lazy": false`.

### Idle unload

Loaded models can be released — freeing their CUDA/VRAM allocations — either on demand or automatically after an idle period. This is useful when the GPU is shared with other workloads and you want resident models to give VRAM back between bursts of requests.

| Key | Scope | Default | Meaning |
|---|---|---:|---|
| `idle_timeout_s` | top-level | `0` | Default idle TTL (seconds) after which a loaded model is unloaded. `0` disables auto-unload. |
| `idle_timeout_s` | per-model | inherit | Per-model override. `>= 0` sets an explicit TTL; `< 0` (or omitted) inherits the top-level default. |
| `reaper_interval_s` | top-level | `10` | How often the background reaper scans for idle models. |
| `idle_exit_after_s` | top-level | `0` | `>0`: once **all** models have unloaded and the server has stayed idle this long, the process `exit(0)`s to fully release the GPU. Requires a container restart policy; set it **above** `idle_timeout_s`. `0` disables. |

The reaper thread starts if at least one model has an effective `idle_timeout_s > 0` **or** `idle_exit_after_s > 0`. Unloading is safe against in-flight requests (it takes the same per-model lock as `run`); a model reloads automatically on its next request (subject to reload latency).

### Idle exit (fully releasing the GPU)

Unloading a model frees its weights/VRAM, but the **CUDA primary context** (~100–200 MiB of driver state + compiled kernels) stays resident for the life of the process — ggml initializes it via one-shot process-lifetime statics, so it cannot be safely torn down and re-created in-process. While that context exists the GPU is held out of its lowest power state (e.g. an A2 sits at P0/~20 W instead of P8/~6 W).

`idle_exit_after_s` closes that gap the only safe way: when the server has fully drained (every model unloaded) and stayed idle past the grace period, it `exit(0)`s. Under a restart policy (`restart: unless-stopped`, Kubernetes, systemd, …) the process comes back **cold** — no context, GPU at idle — and re-initializes on the next request. It's scale-to-zero for the GPU. The exit only fires after a model was loaded at least once, ignores `/health` probes, and won't loop on a never-used server.

> [!NOTE]
> Idle unload releases the model and its session. The first request after an unload pays the model's load cost again; the first request after an idle-exit also pays process startup. Set `idle_timeout_s` above your expected inter-request gap to keep hot models warm, and `idle_exit_after_s` well above `idle_timeout_s` so the GPU is only released when genuinely idle.

## Start

```bash
build/bin/audiocpp_server --config server.json
```

## Endpoints

### `GET /health`

Returns server readiness and the number of configured models.

### `GET /v1/models`

Returns OpenAI-style model entries for the configured audio.cpp model ids. Each entry also reports `"loaded"` (whether the model currently holds resources) and its effective `"idle_timeout_s"`.

### `POST /v1/models/{id}/load`

Eagerly loads a configured model (creates its framework session and allocates device memory) without running a task. Useful for warming a model before first use.

```bash
curl -X POST http://127.0.0.1:8080/v1/models/pocket-tts/load
# {"id":"pocket-tts","loaded":true}
```

### `POST /v1/models/{id}/unload`

Releases a loaded model and its session, freeing its CUDA/VRAM allocations. Safe to call while requests are in flight (serializes on the same per-model lock); the model reloads on its next request. `unloaded` reports whether the model was resident before the call.

```bash
curl -X POST http://127.0.0.1:8080/v1/models/pocket-tts/unload
# {"id":"pocket-tts","unloaded":true,"loaded":false}
```

### `POST /v1/audio/speech`

OpenAI-style text-to-audio. The response is `audio/wav` by default.

```bash
curl http://127.0.0.1:8080/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -o out.wav \
  -d '{
    "model": "pocket-tts",
    "input": "audio.cpp is serving this request through the framework runtime.",
    "voice_ref": "/path/to/reference.wav",
    "max_tokens": 96,
    "seed": 1234
  }'
```

Set `"response_format": "json"` to receive base64 WAV in a JSON response.

If `"voice"` names a **registered voice** (see below) it supplies the reference
sample/transcript (clone) or instruction (design), and the voice's stored `model`
is used when the request omits `"model"`. An unregistered `"voice"` string is
passed through to the model's native handling (e.g. Qwen3 CustomVoice speakers).

### Voice registry — `/v1/voices`

Stores named voices on disk (`<request-dir>/voices/<name>/`) so a voice can be
created once and reused by id. A **clone** voice carries a reference sample (+ its
transcript); a **design** voice carries a text instruction. Names must match
`[A-Za-z0-9_-]` (1–64 chars).

```bash
# create a clone voice from a sample (inline base64, or "sample_path": server-local)
curl -X POST http://127.0.0.1:8080/v1/voices -H 'Content-Type: application/json' -d '{
  "name": "narrator", "model": "qwen3-tts",
  "sample_b64": "<base64 wav>", "reference_text": "transcript of the sample"
}'
# create a design voice from a description
curl -X POST http://127.0.0.1:8080/v1/voices -d '{
  "name": "calm_host", "model": "qwen3-voicedesign", "instruct": "A calm, warm adult narrator"
}'

curl http://127.0.0.1:8080/v1/voices              # list
curl http://127.0.0.1:8080/v1/voices/narrator      # one voice
curl -X DELETE http://127.0.0.1:8080/v1/voices/narrator

# then synthesize by id — no need to resend the sample or pick the model:
curl http://127.0.0.1:8080/v1/audio/speech -o out.wav \
  -d '{"input": "Reused voice.", "voice": "narrator"}'
```

### `POST /v1/audio/transcriptions`

JSON transcription request. Provide the audio as a **server-local path** (`audio` /
`audio_path` / `file`) or **inline** as base64 (`audio_b64`) for callers that can't
drop a file the server can read (a remote client or another container).

```bash
# server-local path:
curl http://127.0.0.1:8080/v1/audio/transcriptions \
  -H 'Content-Type: application/json' \
  -d '{"model": "qwen3-asr", "audio": "/path/to/input.wav"}'

# inline base64 (WAV bytes):
curl http://127.0.0.1:8080/v1/audio/transcriptions \
  -H 'Content-Type: application/json' \
  -d '{"model": "qwen3-asr", "audio_b64": "<base64 wav>"}'
```

### `POST /v1/tasks/run`

Generic framework request route. The `request` object uses the same JSON fields as the `audiocpp_cli` request sequence format.

```bash
curl http://127.0.0.1:8080/v1/tasks/run \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "pocket-tts",
    "request": {
      "text": "Generic audio.cpp request.",
      "voice_ref": "/path/to/reference.wav",
      "max_tokens": 96,
      "seed": 1234
    }
  }'
```
