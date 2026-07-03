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

The reaper thread only starts if at least one model has an effective `idle_timeout_s > 0`. Unloading is safe against in-flight requests (it takes the same per-model lock as `run`); a model reloads automatically on its next request (subject to reload latency).

> [!NOTE]
> Idle unload releases the model and its session. The first request after an unload pays the model's load cost again. Set `idle_timeout_s` above your expected inter-request gap to keep hot models warm.

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

### `POST /v1/audio/transcriptions`

JSON transcription request using a server-local audio path.

```bash
curl http://127.0.0.1:8080/v1/audio/transcriptions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3-asr",
    "audio": "/path/to/input.wav"
  }'
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
