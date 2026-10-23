# Local voice search

Search can recognise a short spoken title on the PS5 using its current user's
microphone input and a local multilingual Whisper tiny model. Selecting the
controller microphone in PS5 microphone settings makes that the source.
SlopFin follows the user's input routing and never unmutes or changes devices.

The first explicit voice-search request downloads about 32 MB over verified
HTTPS. The model's immutable upstream revision, exact size and SHA-256 are
pinned in `src/voice_search.cpp`; redirects are restricted to Hugging Face's
HTTPS hosts. Downloads stream to a private `.partial` file, are bounded by the
expected size, verified before an atomic rename, and discarded on cancellation
or failure. An interrupted download is restarted on the next request. The
verified model lives under `/data/slopfin/voice`, outside the update folder,
and is checked again before loading. Account tokens are never attached to
model requests.

Audio capture starts only after the user requests it and the model is ready.
The AudioIn API is resolved lazily; unsupported firmware or a missing microphone
produces a search error without preventing the rest of the app from launching.
Capture uses 16 kHz signed mono PCM in 256-sample grains. Any nonzero or unknown
platform silent state is treated as muted. Recording ends after 12 seconds,
after 1.2 seconds of quiet following speech, or when the user finishes it.
PCM remains in memory and is cleared after recognition; neither audio nor
recognised text is written to the app's diagnostic log.

Download, microphone input and CPU recognition run on a worker, away from the
render core. Recognition uses four CPU threads, no GPU backend or remote
service. Cancel aborts network/inference work and discards the query. Shutdown
must call `voice_search::shutdown()` before HTTP and platform teardown. The
decoder model is freed after each request so ordinary playback does not retain
its working memory. This quantized tiny model favours download size and speed;
unusual names, short ambiguous phrases and background television may need a
retry or typed correction.

Native model reads use POSIX descriptors, including the dependency's loader
callbacks, because the runtime's C file-reading functions have failed on files
that POSIX reads can access. ggml's aligned tensor, graph and compute arenas
use SlopFin's paired flexible-memory allocation hooks. The remaining C++
vectors and CPU scratch arrays use the normal GPU build's globally wrapped
allocator backed by the SDK platform's direct-memory heap. No model-sized
buffer is loaded through the small ordinary libc heap.

`GPU_MENU=1 make SOFTWARE_AUDIO=1` and `make host` build the pinned static
dependency automatically. Native voice support follows `GPU_MENU` by default;
the legacy CPU-menu developer build leaves it disabled because that SDK lacks
the CPU tokenizer's locale/unwind support. `VOICE_SEARCH=0` is an explicit
native opt-out and `HOST_VOICE_SEARCH=0` disables it in the preview.
Native compilation targets
PS5 AVX2/FMA/F16C and the normal GPU application link recipe. The Linux preview
captures its default SDL audio input and can exercise the same recogniser.
The model is not bundled into the application archive, keeping legacy updater
download limits intact.
