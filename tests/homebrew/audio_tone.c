// ============================================================================
//  PSBET test homebrew — audio_tone.c (Phase 4)
//  AudioOut exercise: plays deterministic tones through the guest-side mixer
//  (solo 440 Hz, then a two-port mix: 660 Hz full volume + 220 Hz half) while
//  presenting one clear frame per submitted chunk so paced runs advance the
//  audio clock with the video. The engine dumps the mixed master stream as a
//  WAV (--audio-dump); the CI suite verifies exact content, sample count,
//  zero drops and the measured latency bound.
//  Exit code 0 = all checks passed.
// ============================================================================
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

#define TONE_LEN_FRAMES 12000        // 0.25 s at 48 kHz per section
#define CHUNK_FRAMES    800          // submitted per presented frame

static void say(const char* s) { sceKernelWrite(1, s, strlen(s)); }
static void fail(const char* why) {
    say("[audio] CHECKS FAILED: ");
    say(why);
    say("\n");
    sceKernelExitProcess(1);
}

// One clear-only display command buffer (keeps the frame clock running).
static __attribute__((aligned(16))) uint8_t dcb[32];
static void present_frame(void) {
    uint32_t* w = (uint32_t*)dcb;
    float*    c = (float*)(dcb + 8);
    w[0] = 1;                      // CmdOp::Clear
    w[1] = 24;                     // header (8) + 16 bytes of color
    c[0] = 0.05f; c[1] = 0.05f; c[2] = 0.08f; c[3] = 1.f;
    void* addr = dcb;
    uint32_t size = 24;
    if (sceGnmSubmitCommandBuffers(1, &addr, &size, 0, 0, 0) != 0)
        fail("submit");
}

// Continuous phase: `base` is the global sample index of this chunk, so the
// waveform is seamless across chunk boundaries (no per-chunk restart).
static void gen_s16_stereo(int16_t* buf, uint32_t frames, float freq,
                           float amp /*0..1*/, uint32_t base) {
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = amp * sinf(freq * 2.f * 3.14159265f *
                                   (float)(base + i) / 48000.f);
        const int16_t s = (int16_t)(v * 32767.f);
        buf[2 * i] = s;
        buf[2 * i + 1] = s;
    }
}

void psb_entry(void) {
    say("[audio] AudioOut demo online\n");

    // ---- section 1: solo 440 Hz on the Main port ---------------------------
    int32_t main_port = sceAudioOutOpen(0, PSB_AUDIO_PORT_MAIN, 0,
                                        CHUNK_FRAMES, 48000,
                                        PSB_AUDIO_FMT_S16_STEREO);
    if (main_port < 0) fail("open main");
    int32_t vol[2] = {PSB_AUDIO_VOL_MAX, PSB_AUDIO_VOL_MAX};
    if (sceAudioOutSetVolume(main_port, PSB_AUDIO_CH_LEFT | PSB_AUDIO_CH_RIGHT,
                             vol) != 0)
        fail("set volume main");

    static int16_t buf[CHUNK_FRAMES * 2];
    for (uint32_t done = 0; done < TONE_LEN_FRAMES; done += CHUNK_FRAMES) {
        gen_s16_stereo(buf, CHUNK_FRAMES, 440.f, 0.7f, done);
        if (sceAudioOutOutput(main_port, buf) != 0) fail("output solo");
        present_frame();
    }

    // ---- section 2: two-port mix (660 full + 220 half) ---------------------
    int32_t bgm_port = sceAudioOutOpen(0, PSB_AUDIO_PORT_BGM, 0,
                                       CHUNK_FRAMES, 48000,
                                       PSB_AUDIO_FMT_S16_STEREO);
    if (bgm_port < 0) fail("open bgm");
    int32_t half[2] = {PSB_AUDIO_VOL_MAX / 2, PSB_AUDIO_VOL_MAX / 2};
    if (sceAudioOutSetVolume(bgm_port, PSB_AUDIO_CH_LEFT | PSB_AUDIO_CH_RIGHT,
                             half) != 0)
        fail("set volume bgm");

    static int16_t bufA[CHUNK_FRAMES * 2];
    static int16_t bufB[CHUNK_FRAMES * 2];
    for (uint32_t done = 0; done < TONE_LEN_FRAMES; done += CHUNK_FRAMES) {
        gen_s16_stereo(bufA, CHUNK_FRAMES, 660.f, 0.5f, done);
        gen_s16_stereo(bufB, CHUNK_FRAMES, 220.f, 0.5f, done);
        if (sceAudioOutOutput(main_port, bufA) != 0) fail("output main mix");
        if (sceAudioOutOutput(bgm_port, bufB) != 0) fail("output bgm mix");
        present_frame();
    }

    if (sceAudioOutClose(main_port) != 0) fail("close main");
    if (sceAudioOutClose(bgm_port) != 0) fail("close bgm");

    // Error-path checks (documented codes, GUEST_ABI §9).
    if (sceAudioOutOpen(0, 9, 0, CHUNK_FRAMES, 48000,
                        PSB_AUDIO_FMT_S16_STEREO) != -1)
        fail("invalid port type accepted");
    if (sceAudioOutOpen(0, PSB_AUDIO_PORT_MAIN, 0, CHUNK_FRAMES, 44100,
                        PSB_AUDIO_FMT_S16_STEREO) != -1)
        fail("invalid rate accepted");
    if (sceAudioOutClose(999) != -1) fail("bogus handle accepted");

    say("[audio] ALL CHECKS PASSED\n");
    sceKernelExitProcess(0);
}
