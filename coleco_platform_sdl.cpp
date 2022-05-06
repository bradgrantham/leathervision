#include <thread>
#include <deque>
#include <chrono>
#include <cassert>

#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif /* EMSCRIPTEN */

#include <SDL2/SDL.h>

#include "coleco_platform.h"

#include "tms9918.h"

namespace PlatformInterface
{

std::deque<Event> event_queue;

bool EventIsWaiting()
{
    return event_queue.size() > 0;
}

Event DequeueEvent()
{
    if(EventIsWaiting()) {
        Event e = event_queue.front();
        event_queue.pop_front();
        return e;
    } else
        return {NONE, 0};
}

static constexpr uint8_t CONTROLLER1_NORTH_BIT = 0x01;
static constexpr uint8_t CONTROLLER1_EAST_BIT = 0x02;
static constexpr uint8_t CONTROLLER1_SOUTH_BIT = 0x04;
static constexpr uint8_t CONTROLLER1_WEST_BIT = 0x08;
static constexpr uint8_t CONTROLLER1_FIRE_LEFT_BIT = 0x40;

static constexpr uint8_t CONTROLLER1_KEYPAD_MASK = 0x0F;
static constexpr uint8_t CONTROLLER1_FIRE_RIGHT_BIT = 0x40;
static constexpr uint8_t CONTROLLER1_KEYPAD_0 = 0x05;
static constexpr uint8_t CONTROLLER1_KEYPAD_1 = 0x02;
static constexpr uint8_t CONTROLLER1_KEYPAD_2 = 0x08;
static constexpr uint8_t CONTROLLER1_KEYPAD_3 = 0x03;
static constexpr uint8_t CONTROLLER1_KEYPAD_4 = 0x0D;
static constexpr uint8_t CONTROLLER1_KEYPAD_5 = 0x0C;
static constexpr uint8_t CONTROLLER1_KEYPAD_6 = 0x01;
static constexpr uint8_t CONTROLLER1_KEYPAD_7 = 0x0A;
static constexpr uint8_t CONTROLLER1_KEYPAD_8 = 0x0E;
static constexpr uint8_t CONTROLLER1_KEYPAD_9 = 0x04;
static constexpr uint8_t CONTROLLER1_KEYPAD_asterisk = 0x06;
static constexpr uint8_t CONTROLLER1_KEYPAD_pound = 0x09;

uint8_t controller_1_joystick_state = 0;
uint8_t controller_2_joystick_state = 0;
uint8_t controller_1_keypad_state = 0;
uint8_t controller_2_keypad_state = 0;

uint8_t GetJoystickState(ControllerIndex controller)
{
    uint8_t data;
    switch(controller) {
        case CONTROLLER_1:
            data = (~controller_1_joystick_state) & 0x7F;
            break;
        case CONTROLLER_2:
            data = (~controller_2_joystick_state) & 0x7F;
            break;
    }
    return data;
}

uint8_t GetKeypadState(ControllerIndex controller)
{
    uint8_t data;
    switch(controller) {
        case CONTROLLER_1:
            data = (~controller_1_keypad_state) & 0x7F;
            break;
        case CONTROLLER_2:
            data = (~controller_2_keypad_state) & 0x7F;
            break;
    }
    return data;
}

SDL_AudioDeviceID audio_device;
bool audio_needs_start = true;
SDL_AudioFormat actual_audio_format;

void EnqueueAudioSamples(uint8_t *buf, size_t sz)
{
    if(audio_needs_start) {
        audio_needs_start = false;
        SDL_PauseAudioDevice(audio_device, 0);
        /* give a little data to avoid gaps and to avoid a pop */
        std::array<uint8_t, 1024> lead_in;
        for(int i = 0; i < lead_in.size(); i++) {
            lead_in[i] = 128 + (buf[0] - 128) * i / lead_in.size();
        }
        SDL_QueueAudio(audio_device, lead_in.data(), lead_in.size());
    }

    if(actual_audio_format == AUDIO_U8) {
        SDL_QueueAudio(audio_device, buf, sz);
    }

}

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Surface *surface;

static constexpr int SCREEN_SCALE = 3;

std::chrono::time_point<std::chrono::system_clock> previous_draw_time;
std::chrono::time_point<std::chrono::system_clock> previous_event_time;

void Start(uint32_t& audioSampleRate, size_t& preferredAudioBufferSampleCount)
{
#if defined(EMSCRIPTEN)

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        SDL_Log("Unable to initialize SDL: %s", SDL_GetError());
        exit(1);
    }

#else /* ! EMSCRIPTEN */

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        SDL_Log("Unable to initialize SDL: %s", SDL_GetError());
        exit(1);
    }

#endif /* EMSCRIPTEN */

    window = SDL_CreateWindow("ColecoVision", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, TMS9918A::SCREEN_X * SCREEN_SCALE, TMS9918A::SCREEN_Y * SCREEN_SCALE, SDL_WINDOW_RESIZABLE);
    if(!window) {
        printf("could not open window\n");
        exit(1);
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if(!renderer) {
        printf("could not create renderer\n");
        exit(1);
    }
    surface = SDL_CreateRGBSurface(0, TMS9918A::SCREEN_X, TMS9918A::SCREEN_Y, 24, 0, 0, 0, 0);
    if(!surface) {
        printf("could not create surface\n");
        exit(1);
    }

    SDL_AudioSpec audiospec{0};
    audiospec.freq = 44100;
    audiospec.format = AUDIO_U8;
    audiospec.channels = 1;
    audiospec.samples = 1024; // audiospec.freq / 100;
    audiospec.callback = nullptr;
    SDL_AudioSpec obtained;

    audio_device = SDL_OpenAudioDevice(nullptr, 0, &audiospec, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE); // | SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    assert(audio_device > 0);

    switch(obtained.format) {
        case AUDIO_U8: {
            /* okay, native format */
            break;
        }
        default:
            printf("unknown audio format chosen: %X\n", obtained.format);
            exit(1);
    }

    audioSampleRate = obtained.freq;
    preferredAudioBufferSampleCount = obtained.samples / 2;
    actual_audio_format = obtained.format;

    SDL_PumpEvents();

    previous_event_time = previous_draw_time = std::chrono::system_clock::now();
}

bool shift_pressed = false;

static void HandleEvents(void)
{
    auto set_bits = [](uint8_t& data, uint8_t bits) { data = data | bits; };
    auto clear_bits = [](uint8_t& data, uint8_t bits) { data = data & ~bits; };
    auto set_bitfield = [](uint8_t& data, uint8_t mask, uint8_t bits) { data = (data & ~mask) | bits; };

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_WINDOWEVENT:
                printf("window event %d\n", event.window.event);
                switch(event.window.event) {
                }
                break;
            case SDL_QUIT:
                event_queue.push_back({QUIT, 0});
                break;

            case SDL_KEYDOWN:
                switch (event.key.keysym.scancode) {
                    case SDL_SCANCODE_RSHIFT:
                    case SDL_SCANCODE_LSHIFT:
                        shift_pressed = true;
                        break;
                    case SDL_SCANCODE_W:
                        set_bits(controller_1_joystick_state, CONTROLLER1_NORTH_BIT);
                        break;
                    case SDL_SCANCODE_A:
                        set_bits(controller_1_joystick_state, CONTROLLER1_WEST_BIT);
                        break;
                    case SDL_SCANCODE_S:
                        set_bits(controller_1_joystick_state, CONTROLLER1_SOUTH_BIT);
                        break;
                    case SDL_SCANCODE_D:
                        set_bits(controller_1_joystick_state, CONTROLLER1_EAST_BIT);
                        break;
                    case SDL_SCANCODE_SPACE:
                        set_bits(controller_1_joystick_state, CONTROLLER1_FIRE_LEFT_BIT);
                        break;
                    case SDL_SCANCODE_RETURN:
                        set_bits(controller_1_keypad_state, CONTROLLER1_FIRE_RIGHT_BIT);
                        break;
                    case SDL_SCANCODE_0:
                        set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_0);
                        break;
                    case SDL_SCANCODE_1:
                        set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_1);
                        break;
                    case SDL_SCANCODE_2:
                        set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_2);
                        break;
                    case SDL_SCANCODE_3:
                        if(shift_pressed) {
                            set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_pound);
                        } else {
                            set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_3);
                        }
                        break;
                    case SDL_SCANCODE_4:
                        set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_4);
                        break;
                    case SDL_SCANCODE_5:
                        set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_5);
                        break;
                    case SDL_SCANCODE_6:
                        set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_6);
                        break;
                    case SDL_SCANCODE_7:
                        set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_7);
                        break;
                    case SDL_SCANCODE_8:
                        if(shift_pressed) {
                            set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_asterisk);
                        } else {
                            set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_8);
                        }
                        break;
                    case SDL_SCANCODE_9:
                        set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_9);
                        break;
                    default:
                        break;
                }
                break;
            case SDL_KEYUP:
                switch (event.key.keysym.scancode) {
                    case SDL_SCANCODE_V:
                        event_queue.push_back({SAVE_VDP_STATE, 0});
                        break;
                    case SDL_SCANCODE_N:
                        event_queue.push_back({DEBUG_VDP_WRITES, 0});
                        break;
                    case SDL_SCANCODE_R:
                        event_queue.push_back({RESET, 0});
                        break;
                    case SDL_SCANCODE_RSHIFT:
                    case SDL_SCANCODE_LSHIFT:
                        shift_pressed = false;
                        break;
                    case SDL_SCANCODE_W:
                        clear_bits(controller_1_joystick_state, CONTROLLER1_NORTH_BIT);
                        break;
                    case SDL_SCANCODE_A:
                        clear_bits(controller_1_joystick_state, CONTROLLER1_WEST_BIT);
                        break;
                    case SDL_SCANCODE_S:
                        clear_bits(controller_1_joystick_state, CONTROLLER1_SOUTH_BIT);
                        break;
                    case SDL_SCANCODE_D:
                        clear_bits(controller_1_joystick_state, CONTROLLER1_EAST_BIT);
                        break;
                    case SDL_SCANCODE_SPACE:
                        clear_bits(controller_1_joystick_state, CONTROLLER1_FIRE_LEFT_BIT);
                        break;
                    case SDL_SCANCODE_RETURN:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_FIRE_RIGHT_BIT);
                        break;
                    case SDL_SCANCODE_0:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_1:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_2:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_3:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_4:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_5:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_6:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_7:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_8:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    case SDL_SCANCODE_9:
                        clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }
}

void Frame(const uint8_t* vdp_registers, const uint8_t* vdp_ram, uint8_t& vdp_status_result, [[maybe_unused]] float megahertz)
{
    using namespace std::chrono_literals;

    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);

    uint8_t* framebuffer = reinterpret_cast<uint8_t*>(surface->pixels);

    auto pixel_setter = [framebuffer](int x, int y, uint8_t color) {
        uint8_t *pixel = framebuffer + 4 * (x + y * TMS9918A::SCREEN_X) + 0;
        TMS9918A::CopyColor(pixel, TMS9918A::Colors[color]);
    };

    vdp_status_result = TMS9918A::CreateImageAndReturnFlags(vdp_registers, vdp_ram, pixel_setter);

    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);

    std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
    std::chrono::duration<float> elapsed;
    
    elapsed = now - previous_event_time;
    if(elapsed.count() > .05) {
        HandleEvents();
        previous_event_time = now;
    }

    elapsed = now - previous_draw_time;
    if(elapsed.count() > .05) {
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        if(!texture) {
            printf("could not create texture\n");
            exit(1);
        }
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_DestroyTexture(texture);
        previous_draw_time = now;
    }
}

#if defined(EMSCRIPTEN)
void caller(void *f_)
{
    std::function<bool()> *f = (std::function<bool()>*)f_;

    bool quit = (*f)();
    if(quit) {
        emscripten_cancel_main_loop();
    }
}
#endif /* EMSCRIPTEN */ 

void MainLoopAndShutdown(MainLoopBodyFunc body)
{
#if defined(EMSCRIPTEN)

    std::function<bool()> body_for_emscripten = [&]()->bool{ return body(); };
    emscripten_set_main_loop_arg(caller, &body_for_emscripten, 0, 1);

#else /* !EMSCRIPTEN */

    bool quit_requested = false;
    while(!quit_requested)
    {
        quit_requested = body();
    }

    SDL_Quit();

#endif /* EMSCRIPTEN */

}

};


