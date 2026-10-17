// ============================================================================
//  PSBET — audio_sdl.cpp (Phase 4, optional PSBET_WITH_SDL3 build)
//  Real SDL3 playback backend. The mixer pushes mixed master frames into an
//  SDL audio stream; the device drains them at wall clock (or the CI dummy
//  driver drains instantly). Format conversion is SDL's job — we hand it
//  exactly the master domain (48 kHz stereo s16).
// ============================================================================
#include "psbet/common.h"

#ifdef PSBET_WITH_SDL3

#include <atomic>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>

#include "core/audio.h"
#include "core/log.h"

namespace psb::audio {

struct SdlBackend::Impl {
    SDL_AudioStream* stream = nullptr;
    std::mutex mtx;
    u64 pushed = 0;
};

SdlBackend::~SdlBackend() { stop(); }

bool SdlBackend::start(u32 rate, u32 channels) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_WARN("audio-sdl: SDL_InitSubSystem(audio) failed: %s", SDL_GetError());
        return false;
    }
    impl_ = new Impl;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = static_cast<int>(channels);
    spec.freq = static_cast<int>(rate);
    // No callback: pull-free push model via SDL_PutAudioStreamData.
    impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                              &spec, nullptr, nullptr);
    if (!impl_->stream) {
        LOG_WARN("audio-sdl: open playback stream failed: %s", SDL_GetError());
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    SDL_ResumeAudioStreamDevice(impl_->stream);
    return true;
}

void SdlBackend::stop() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (impl_->stream) SDL_DestroyAudioStream(impl_->stream);
        impl_->stream = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SdlBackend::feed(const i16* interleaved, u32 frames) {
    if (!impl_ || !impl_->stream) return;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    const size_t bytes = static_cast<size_t>(frames) * 2 * sizeof(i16);
    if (!SDL_PutAudioStreamData(impl_->stream, interleaved,
                                static_cast<int>(bytes))) {
        LOG_WARN("audio-sdl: PutAudioStreamData failed: %s", SDL_GetError());
        return;
    }
    impl_->pushed += frames;
}

u32 SdlBackend::pendingFrames() const {
    if (!impl_ || !impl_->stream) return 0;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    const int bytes = SDL_GetAudioStreamAvailable(impl_->stream);
    return bytes > 0 ? static_cast<u32>(bytes) / 4 /* stereo s16 */ : 0;
}

} // namespace psb::audio

#endif // PSBET_WITH_SDL3
