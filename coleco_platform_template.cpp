#include <deque>
#include <thread>
#include <chrono>

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

constexpr uint8_t CONTROLLER1_NORTH_BIT = 0x01;
constexpr uint8_t CONTROLLER1_EAST_BIT = 0x02;
constexpr uint8_t CONTROLLER1_SOUTH_BIT = 0x04;
constexpr uint8_t CONTROLLER1_WEST_BIT = 0x08;
constexpr uint8_t CONTROLLER1_FIRE_LEFT_BIT = 0x40;

constexpr uint8_t CONTROLLER1_KEYPAD_MASK = 0x0F;
constexpr uint8_t CONTROLLER1_FIRE_RIGHT_BIT = 0x40;
constexpr uint8_t CONTROLLER1_KEYPAD_0 = 0x05;
constexpr uint8_t CONTROLLER1_KEYPAD_1 = 0x02;
constexpr uint8_t CONTROLLER1_KEYPAD_2 = 0x08;
constexpr uint8_t CONTROLLER1_KEYPAD_3 = 0x03;
constexpr uint8_t CONTROLLER1_KEYPAD_4 = 0x0D;
constexpr uint8_t CONTROLLER1_KEYPAD_5 = 0x0C;
constexpr uint8_t CONTROLLER1_KEYPAD_6 = 0x01;
constexpr uint8_t CONTROLLER1_KEYPAD_7 = 0x0A;
constexpr uint8_t CONTROLLER1_KEYPAD_8 = 0x0E;
constexpr uint8_t CONTROLLER1_KEYPAD_9 = 0x04;
constexpr uint8_t CONTROLLER1_KEYPAD_asterisk = 0x06;
constexpr uint8_t CONTROLLER1_KEYPAD_pound = 0x09;

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

int GetAudioSampleRate()
{
    return 11050;
}

size_t GetPreferredAudioBufferSampleCount()
{
    return 11050 / 16;
}

void EnqueueAudioSamples(uint8_t *buf, size_t sz)
{
    printf("enqueue %zd audio samples\n", sz);
}

uint8_t framebuffer[TMS9918A::SCREEN_X * TMS9918A::SCREEN_Y * 3];

std::chrono::time_point<std::chrono::system_clock> then;

std::thread* input_thread;

void get_input(void)
{
    using namespace std::chrono_literals;
    auto set_bits = [](uint8_t& data, uint8_t bits) { data = data | bits; };
    [[maybe_unused]] auto clear_bits = [](uint8_t& data, uint8_t bits) { data = data & ~bits; };
    auto set_bitfield = [](uint8_t& data, uint8_t mask, uint8_t bits) { data = (data & ~mask) | bits; };
    auto press_duration = 50ms;

    while(1) {
        int f = getchar();
        switch(f) {
            case 'q': 
                event_queue.push_back({QUIT, 0});
                return;
            case 'w':
                set_bits(controller_1_joystick_state, CONTROLLER1_NORTH_BIT);
                std::this_thread::sleep_for(press_duration); controller_1_joystick_state = 0;
                break;
            case 'a':
                set_bits(controller_1_joystick_state, CONTROLLER1_WEST_BIT);
                std::this_thread::sleep_for(press_duration); controller_1_joystick_state = 0;
                break;
            case 's':
                set_bits(controller_1_joystick_state, CONTROLLER1_SOUTH_BIT);
                std::this_thread::sleep_for(press_duration); controller_1_joystick_state = 0;
                break;
            case 'd':
                set_bits(controller_1_joystick_state, CONTROLLER1_EAST_BIT);
                std::this_thread::sleep_for(press_duration); controller_1_joystick_state = 0;
                break;
            case ' ':
                set_bits(controller_1_joystick_state, CONTROLLER1_FIRE_LEFT_BIT);
                std::this_thread::sleep_for(press_duration); controller_1_joystick_state = 0;
                break;
            case '.':
                set_bits(controller_1_keypad_state, CONTROLLER1_FIRE_RIGHT_BIT);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '0':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_0);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '1':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_1);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '2':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_2);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '3':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_pound);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '#':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_3);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '4':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_4);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '5':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_5);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '6':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_6);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '7':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_7);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '*':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_asterisk);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '8':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_8);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
            case '9':
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_9);
                std::this_thread::sleep_for(press_duration); controller_1_keypad_state = 0;
                break;
        }
    }
}

void Start()
{
    input_thread = new std::thread(get_input);
    then = std::chrono::system_clock::now();
    printf("\033[J");
}

int frameCount = 0;

// https://stackoverflow.com/a/34571089/211234
static const char *BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
void base64Encode(const char *in, int count) {
    int val = 0;
    int valb = -6;
    int bytesWritten = 0;

    for (int i = 0; i < count; i++) {
        char c = in[i];
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            putchar(BASE64_ALPHABET[(val >> valb) & 0x3F]);
            bytesWritten ++;
            valb -= 6;
        }
    }
    if (valb > -6) {
        putchar(BASE64_ALPHABET[((val << 8) >> (valb + 8)) & 0x3F]);
        bytesWritten ++;
    }
    while (bytesWritten % 4 != 0) {
        putchar('=');
        bytesWritten ++;
    }
}


void display_frame()
{
    if(false) {
        for(int row = 0; row < 48; row++){
            for(int col = 0; col < 160; col++){
                int x = col * TMS9918A::SCREEN_X / 160;
                int y = row * TMS9918A::SCREEN_Y / 48;
                uint8_t *pixel = framebuffer + 3 * (x + y * TMS9918A::SCREEN_X) + 0;
                int intensity = pixel[0] * .33 + pixel[1] * .34 + pixel[2] * .33;
                putchar(" .-oO@"[intensity * 5 / 256]);
            }
            puts("");
        }
    } else {
        static char buffer[TMS9918A::SCREEN_X * TMS9918A::SCREEN_Y * 3 + 128]; // XXX image plus header
        int bytesToEncode = sprintf(buffer, "P6 %d %d 255\n", TMS9918A::SCREEN_X, TMS9918A::SCREEN_Y);
        memcpy(buffer + bytesToEncode, framebuffer, sizeof(framebuffer));
        bytesToEncode += sizeof(framebuffer);

        printf("\033]1337;File=width=50%%;inline=1:");
        base64Encode(buffer, bytesToEncode);
        printf("\007\n");
        fflush(stdout);
    }
}

void Frame(const uint8_t* vdp_registers, const uint8_t* vdp_ram, uint8_t& vdp_status_result, [[maybe_unused]] float megahertz)
{
    using namespace std::chrono_literals;

    auto pixel_setter = [](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t *pixel = framebuffer + 3 * (x + y * TMS9918A::SCREEN_X) + 0;
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
    };

    vdp_status_result = TMS9918A::CreateImageAndReturnFlags(vdp_registers, vdp_ram, pixel_setter);

    printf("\033[Hframe %d\n", frameCount++);
    if(frameCount % 10 == 0) {
        display_frame();
    }

    std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
    auto elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(now - then);
    std::this_thread::sleep_for(16666us - elapsed_micros); // 60Hz

    then = now;
}

void MainLoopAndShutdown(MainLoopBodyFunc body)
{
    bool quit_requested = false;
    while(!quit_requested)
    {
        quit_requested = body();
    }
}

};


