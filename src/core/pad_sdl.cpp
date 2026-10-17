// ============================================================================
//  PSBET — pad_sdl.cpp (Phase 4, optional PSBET_WITH_SDL3 build)
//  SDL3 game-controller + sensor driver: real DualSense mapping (buttons,
//  sticks, analog triggers, accelerometer + gyroscope) through SDL's gamepad
//  API. Under the CI dummy driver (or without hardware) pads report
//  disconnected — which is itself a correct, testable state.
// ============================================================================
#include "psbet/common.h"

#ifdef PSBET_WITH_SDL3

#include <mutex>

#include <SDL3/SDL.h>

#include "core/log.h"
#include "core/pad.h"

namespace psb::pad {

namespace {
// SDL semantic gamepad button -> PSBET pad bit (DualSense placement:
// South=✕, East=○, West=□, North=△ under SDL's SONY-hardware mapping).
u32 mapButton(int b) {
    switch (static_cast<SDL_GamepadButton>(b)) {
    case SDL_GAMEPAD_BUTTON_SOUTH:          return PadBtnCross;
    case SDL_GAMEPAD_BUTTON_EAST:           return PadBtnCircle;
    case SDL_GAMEPAD_BUTTON_WEST:           return PadBtnSquare;
    case SDL_GAMEPAD_BUTTON_NORTH:          return PadBtnTriangle;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return PadBtnL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return PadBtnR1;
    case SDL_GAMEPAD_BUTTON_START:          return PadBtnOptions;
    case SDL_GAMEPAD_BUTTON_BACK:           return PadBtnShare;
    case SDL_GAMEPAD_BUTTON_GUIDE:          return PadBtnPs;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:       return PadBtnTouchpad;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:        return PadBtnUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return PadBtnDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return PadBtnLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return PadBtnRight;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return PadBtnL3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return PadBtnR3;
    default:                                return 0;
    }
}

u8 mapAxis(Sint16 v) {   // SDL axis -32768..32767 -> 0..255, center 128
    const int c = (static_cast<int>(v) + 32768) / 257;
    return static_cast<u8>(c < 0 ? 0 : (c > 255 ? 255 : c));
}
} // namespace

struct SdlGamepadDriver::Impl {
    std::mutex mtx;
    SDL_Gamepad* pads[kMaxHandles] = {};
};

SdlGamepadDriver::SdlGamepadDriver() : impl_(new Impl) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        LOG_WARN("pad-sdl: SDL_InitSubSystem(gamepad) failed: %s", SDL_GetError());
        return;
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count && i < static_cast<int>(kMaxHandles); ++i)
            impl_->pads[i] = SDL_OpenGamepad(ids[i]);
        SDL_free(ids);
    }
    LOG_INFO("pad-sdl: %d gamepad(s) attached", count);
}

SdlGamepadDriver::~SdlGamepadDriver() {
    for (SDL_Gamepad* p : impl_->pads)
        if (p) SDL_CloseGamepad(p);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    delete impl_;
    impl_ = nullptr;
}

void SdlGamepadDriver::poll(PadSnapshot* snapshots, u64) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (u32 i = 0; i < kMaxHandles; ++i) {
        SDL_Gamepad* gp = impl_->pads[i];
        PadSnapshot& s = snapshots[i];
        if (!gp || !SDL_GamepadConnected(gp)) {
            s = PadSnapshot{};   // disconnected — correct, testable state
            continue;
        }
        PadState& st = s.state;
        st.connected = 1;

        u32 mask = 0;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
            if (SDL_GetGamepadButton(gp, static_cast<SDL_GamepadButton>(b)))
                mask |= mapButton(b);
        st.buttons = mask;

        st.lx = mapAxis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
        st.ly = mapAxis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
        st.rx = mapAxis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
        st.ry = mapAxis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
        st.l2 = mapAxis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        st.r2 = mapAxis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

        float data[3] = {0, 0, 0};
        if (SDL_GetGamepadSensorData(gp, SDL_SENSOR_ACCEL, data, 3)) {
            st.accelX = data[0]; st.accelY = data[1]; st.accelZ = data[2];
        }
        if (SDL_GetGamepadSensorData(gp, SDL_SENSOR_GYRO, data, 3)) {
            st.gyroX = data[0]; st.gyroY = data[1]; st.gyroZ = data[2];
        }
    }
}

} // namespace psb::pad

#endif // PSBET_WITH_SDL3
