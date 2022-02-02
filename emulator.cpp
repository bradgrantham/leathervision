#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <string>
#include <map>
#include <array>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/i2c-dev.h>
#endif

#include <ao/ao.h>

#include "emulator.h"

#include "z80emu.h"
#include "readhex.h"

#if defined(__linux__)
#include <GL/glew.h>
#endif // defined(__linux__)

#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

#include "gl_utility.h"
#include "coleco_interface.h"

#define PROVIDE_DEBUGGER

constexpr unsigned int DEBUG_NONE = 0x00;
constexpr unsigned int DEBUG_ROM = 0x01;
constexpr unsigned int DEBUG_RAM = 0x02;
constexpr unsigned int DEBUG_IO = 0x04;
constexpr unsigned int DEBUG_SCANOUT = 0x08;
constexpr unsigned int DEBUG_VDP_OPERATIONS = 0x10;
bool save_vdp = false;
unsigned int debug = DEBUG_NONE;
bool abort_on_exception = false;
bool do_save_images_on_vdp_write = false;
const bool break_on_unknown_address = true;
const bool profiling = false;

namespace COLECOinterface
{

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

ao_device *aodev;
constexpr int audio_rate = 44100;

ao_device *open_ao(int rate)
{
    ao_device *device;
    ao_sample_format format;
    int default_driver;

    ao_initialize();

    default_driver = ao_default_driver_id();

    memset(&format, 0, sizeof(format));
    format.bits = 8;
    format.channels = 1;
    format.rate = rate;
    format.byte_format = AO_FMT_LITTLE;

    /* -- Open driver -- */
    device = ao_open_live(default_driver, &format, NULL /* no options */);
    if (device == NULL) {
        fprintf(stderr, "Error opening libao audio device.\n");
        return nullptr;
    }
    return device;
}

void Start()
{
    aodev = open_ao(audio_rate);
    if(aodev == NULL)
        exit(EXIT_FAILURE);
}

int GetAudioSampleRate()
{
    return audio_rate;
}

size_t GetPreferredAudioBufferSampleCount()
{
    return audio_rate / 100;
}

void EnqueueAudioSamples(uint8_t *buf, size_t sz)
{
    ao_play(aodev, (char*)buf, sz);
}

};

Z80_STATE z80state;
bool Z80_INTERRUPT_FETCH = false;
unsigned short Z80_INTERRUPT_FETCH_DATA;

typedef long long clk_t;

constexpr clk_t machine_clock_rate = 3579545;
constexpr uint32_t slice_frequency = 60;
constexpr uint32_t clocks_per_slice = machine_clock_rate / slice_frequency;
constexpr clk_t micros_per_slice = 1000000 / slice_frequency;
volatile bool run_fast = false;
volatile bool pause_cpu = false;

std::vector<board_base*> boards;

bool quit = false;

const int SCREEN_X = 256;
const int SCREEN_Y = 192;
const int SCREEN_SCALE = 3;

unsigned char framebuffer[SCREEN_X * SCREEN_Y * 4];

void write_image(unsigned char image[SCREEN_X * SCREEN_Y * 4], FILE *fp)
{
    fprintf(fp, "P6 %d %d 255\n", SCREEN_X, SCREEN_Y);
    for(int row = 0; row < SCREEN_Y; row++) {
        for(int col = 0; col < SCREEN_X; col++) {
            fwrite(image + (row * SCREEN_X + col) * 4, 3, 1, fp);
        }
    }
}

typedef std::function<void (uint8_t *audiobuffer, size_t dist)> audio_flush_func;

struct SN76489A
{
    bool debug{false};
    unsigned int clock_rate{0};

    int phase = 0;

    unsigned char cmd_latched = 0;
    static constexpr int CMD_BIT = 0x80;
    static constexpr int CMD_REG_MASK = 0x70;
    static constexpr int DATA_MASK = 0x0F;
    static constexpr int CMD_REG_SHIFT = 4;
    static constexpr int FREQ_HIGH_SHIFT = 4;
    static constexpr int FREQ_HIGH_MASK = 0x3F;
    static constexpr int CMD_NOISE_CONFIG_MASK = 0x04;
    static constexpr int CMD_NOISE_CONFIG_SHIFT = 2;
    static constexpr int CMD_NOISE_FREQ_MASK = 0x03;

    int sample_rate;

    unsigned int tone_lengths[3] = {0, 0, 0};
    unsigned int tone_attenuation[3] = {0, 0, 0};

    unsigned int noise_config{0};
    unsigned int noise_length{0};
    unsigned int noise_length_id{0};
    unsigned int noise_attenuation{0};

    unsigned int tone_counters[3] = {0, 0, 0};
    unsigned int tone_bit[3] = {0, 0, 0};
    unsigned int noise_counter{0};

    uint16_t noise_register = 0x8000;
    unsigned int noise_flipflop = 0;

    clk_t previous_clock{0};

    clk_t max_audio_forward;
    size_t audio_buffer_size;
    std::vector<uint8_t> audio_buffer;
    clk_t audio_buffer_next_sample{0};

    SN76489A(unsigned int clock_rate, int sample_rate, size_t audio_buffer_size) :
        clock_rate(clock_rate),
        sample_rate(sample_rate),
        audio_buffer_size(audio_buffer_size)
    {
        max_audio_forward = machine_clock_rate / sample_rate - 1;
        audio_buffer.resize(audio_buffer_size);

        for(int i = 0; i < 3; i++) {
            tone_counters[i] = 0;
            tone_bit[i] = 0;
        }
        noise_counter = 0;
        previous_clock = 0;
    }

    void write(unsigned char data)
    {
        if(debug) printf("sound write 0x%02X\n", data);
        if(data & CMD_BIT) {

            cmd_latched = data;

            unsigned int reg = (data & CMD_REG_MASK) >> CMD_REG_SHIFT;

            if(reg == 1 || reg == 3 || reg == 5) {
                tone_attenuation[(reg - 1) / 2] = data & DATA_MASK;
            } else if(reg == 7) {
                noise_attenuation = data & DATA_MASK;
            } else if(reg == 6) {
                noise_config = (data & CMD_NOISE_CONFIG_MASK) >> CMD_NOISE_CONFIG_SHIFT;
                noise_length_id = data & CMD_NOISE_FREQ_MASK;
                if(noise_length_id == 0) {
                    noise_length = 512;
                } else if(noise_length_id == 1) {
                    noise_length = 1024;
                } else if(noise_length_id == 2) {
                    noise_length = 2048;
                }
                /* if noise_length_id == 3 then noise counter is tone_counters[2]*/

                noise_register = 0x8000;
            }

        } else {

            unsigned int reg = (cmd_latched & CMD_REG_MASK) >> CMD_REG_SHIFT;

            if(reg == 0 || reg == 2 || reg == 4) {
                tone_lengths[reg / 2] = 16 * (((data & FREQ_HIGH_MASK) << FREQ_HIGH_SHIFT) | (cmd_latched & DATA_MASK));
                if(tone_counters[reg / 2] >= tone_lengths[reg / 2])
                    tone_counters[reg / 2] = 0;
            }
        }
    }

    clk_t calc_flip_count(clk_t previous_clock, clk_t current_clock, unsigned int previous_counter, unsigned int length)
    {
        if(length < 1) { 
            return 0;
        }
        clk_t clocks = current_clock - previous_clock;
        clk_t flips = (previous_counter + clocks) / length;
        return flips;
    }

    void advance_noise_to_clock(clk_t flips)
    {
        for(int i = 0; i < flips; i++) {
            noise_flipflop ^= 1;

            if(noise_flipflop) {
                int noise_bit = noise_register & 0x1;
                int new_bit;

                if(noise_config == 1) {
                    new_bit = (noise_register & 0x1) ^ ((noise_register & 0x8) >> 3);
                } else {
                    new_bit = noise_bit;
                }

                noise_register = (noise_register >> 1) | (new_bit << 15);
            }
        }
    }

    void advance_to_clock(clk_t clk)
    {
        clk_t tone_flips[3];
        
        for(int i = 0; i < 3; i++) {
            tone_flips[i] = calc_flip_count(previous_clock, clk, tone_counters[i], tone_lengths[i]);
        }

        int flips;
        if(noise_length_id == 3)  {
            flips = tone_flips[2];
        } else {
            flips = calc_flip_count(previous_clock, clk, noise_counter, noise_length);
        }
        advance_noise_to_clock(flips);

        for(int i = 0; i < 3; i++) {
            tone_bit[i] = tone_bit[i] ^ (tone_flips[i] & 0x1);
            if(tone_lengths[i] > 0) {
                tone_counters[i] = (tone_counters[i] + (clk - previous_clock)) % tone_lengths[i];
            }
        }
        if(noise_length > 0) {
            noise_counter = (noise_counter + (clk - previous_clock)) % noise_length;
        }

        previous_clock = clk;
    }

    static uint8_t scale_by_attenuation_flags(unsigned int att, uint8_t value)
    {
        const static uint16_t att_table[] = {
            256, 203, 161, 128, 101, 80, 64, 51, 40, 32, 25, 20, 16, 12, 10, 0,
        };

        return value * att_table[att] / 256;
    }

    uint8_t get_level()
    {
        uint8_t v =
            scale_by_attenuation_flags(tone_attenuation[0], tone_bit[0] ? 0 : 64) + 
            + scale_by_attenuation_flags(tone_attenuation[1], tone_bit[1] ? 0 : 64)
            + scale_by_attenuation_flags(tone_attenuation[2], tone_bit[2] ? 0 : 64)
            + scale_by_attenuation_flags(noise_attenuation, (noise_register & 0x1) ? 0 : 64);
            ;

        return v;
    }

    void generate_audio(clk_t clk, audio_flush_func audio_flush)
    {
	clk_t current_audio_sample = previous_clock * sample_rate / clock_rate;
        for(clk_t c = previous_clock + 1; c < clk; c += max_audio_forward) {

            clk_t next_audio_sample = (c + 1) * sample_rate / clock_rate;

            if(next_audio_sample > current_audio_sample) {
                advance_to_clock(c);

                audio_buffer[audio_buffer_next_sample++] = get_level();

                if(audio_buffer_next_sample == audio_buffer_size) {
                    audio_flush(audio_buffer.data(), audio_buffer_size);
                    audio_buffer_next_sample = 0;
                }
            }
	    current_audio_sample = next_audio_sample;
        }

        previous_clock = clk;
    }
};

namespace TMS9918A
{

namespace constants
{

static constexpr int REG_A0_A5_MASK = 0x3F;
static constexpr int CMD_MASK = 0xC0;
static constexpr int CMD_SET_REGISTER = 0x80;
static constexpr int CMD_SET_WRITE_ADDRESS = 0x40;
static constexpr int CMD_SET_READ_ADDRESS = 0x00;

static constexpr int VR0_M3_MASK = 0x02;
[[maybe_unused]] static constexpr int VR0_EXTVID_MASK = 0x01;

[[maybe_unused]] static constexpr int VR1_16K_MASK = 0x80; 
static constexpr int VR1_BLANK_MASK = 0x40; /* and BLANK is active low */
static constexpr int VR1_INT_MASK = 0x20;
static constexpr int VR1_M2_MASK = 0x10;
static constexpr int VR1_M1_MASK = 0x08;
static constexpr int VR1_SIZE4_MASK = 0x02;
static constexpr int VR1_MAG2X_MASK = 0x01;

static constexpr int VR2_NAME_TABLE_MASK = 0x0F;
static constexpr int VR2_NAME_TABLE_SHIFT = 10;

static constexpr int VR3_COLORTABLE_MASK_STANDARD = 0xFF;
static constexpr int VR3_COLORTABLE_SHIFT_STANDARD = 6;

static constexpr int VR3_COLORTABLE_MASK_BITMAP = 0x80;
static constexpr int VR3_COLORTABLE_SHIFT_BITMAP = 6;

static constexpr int VR3_ADDRESS_MASK_BITMAP = 0x7F;
static constexpr int VR3_ADDRESS_MASK_SHIFT = 6;

static constexpr int VR4_PATTERN_MASK_STANDARD = 0x07;
static constexpr int VR4_PATTERN_SHIFT_STANDARD = 11;

static constexpr int VR4_PATTERN_MASK_BITMAP = 0x04;
static constexpr int VR4_PATTERN_SHIFT_BITMAP = 11;

static constexpr int VR5_SPRITE_ATTR_MASK = 0x7F;
static constexpr int VR5_SPRITE_ATTR_SHIFT = 7;

static constexpr int VR6_SPRITE_PATTERN_MASK = 0x07;
static constexpr int VR6_SPRITE_PATTERN_SHIFT = 11;

static constexpr int VR7_BD_MASK = 0x0F;
static constexpr int VR7_BD_SHIFT = 0;

static constexpr int VDP_STATUS_F_BIT = 0x80;
static constexpr int VDP_STATUS_5S_BIT = 0x40;
static constexpr int VDP_STATUS_C_BIT = 0x20;

static constexpr int ROW_SHIFT = 5;
static constexpr int THIRD_SHIFT = 11;
static constexpr int CHARACTER_PATTERN_SHIFT = 3;
static constexpr int CHARACTER_COLOR_SHIFT = 3;
static constexpr int ADDRESS_MASK_FILL = 0x3F;

static constexpr int SPRITE_EARLY_CLOCK_MASK = 0x80;
static constexpr int SPRITE_COLOR_MASK = 0x0F;
static constexpr int SPRITE_NAME_SHIFT = 3;
static constexpr int SPRITE_NAME_MASK_SIZE4 = 0xFC;

static constexpr int TRANSPARENT_COLOR_INDEX = 0;

static constexpr int REGISTER_COUNT = 8;

};

typedef std::array<uint8_t, constants::REGISTER_COUNT> register_file_t;

enum GraphicsMode { GRAPHICS_I, GRAPHICS_II, TEXT, MULTICOLOR, UNDEFINED };

bool SpritesAreSize4(const register_file_t& registers)
{
    return registers[1] & constants::VR1_SIZE4_MASK;
}

bool SpritesAreMagnified2X(const register_file_t& registers)
{
    return registers[1] & constants::VR1_MAG2X_MASK;
}

bool ActiveDisplayAreaIsBlanked(const register_file_t& registers)
{
    return (registers[1] & constants::VR1_BLANK_MASK) == 0;
}

uint8_t GetBackdropColor(const register_file_t& registers)
{
    return (registers[7] & constants::VR7_BD_MASK) >> constants::VR7_BD_SHIFT;
}

bool InterruptsAreEnabled(const register_file_t& registers)
{
    return registers[1] & constants::VR1_INT_MASK;
}

bool VSyncInterruptHasOccurred(uint8_t status_register)
{
    return status_register & constants::VDP_STATUS_F_BIT;
}

GraphicsMode GetGraphicsMode(const register_file_t& registers)
{
    bool M1 = registers[1] & constants::VR1_M1_MASK;
    bool M2 = registers[1] & constants::VR1_M2_MASK;
    bool M3 = registers[0] & constants::VR0_M3_MASK;

    if(!M1 && !M2 && !M3) {
        return GraphicsMode::GRAPHICS_I;
    } else if(!M1 && !M2 && M3) {
        return GraphicsMode::GRAPHICS_II;
    } else if(!M1 && M2 && !M3) {
        return GraphicsMode::MULTICOLOR;
    } else if(M1 && !M2 && !M3) {
        return GraphicsMode::TEXT;
    }
    return GraphicsMode::UNDEFINED;
}

bool SpritesVisible(const register_file_t& registers)
{
    if(ActiveDisplayAreaIsBlanked(registers)) {
        return false;
    }

    switch(GetGraphicsMode(registers)) {
        case GRAPHICS_I:
            return true;
            break;
        case GRAPHICS_II:
            return true;
            break;
        case TEXT:
            return false;
            break;
        case MULTICOLOR:
            return true;
            break;
        case UNDEFINED:
            return true;
            break;
    }
    return false;
}

uint16_t GetNameTableBase(const register_file_t& registers)
{
    return (registers[2] & constants::VR2_NAME_TABLE_MASK) << constants::VR2_NAME_TABLE_SHIFT;
}

uint16_t GetSpriteAttributeTableBase(const register_file_t& registers)
{
    return (registers[5] & constants::VR5_SPRITE_ATTR_MASK) << constants::VR5_SPRITE_ATTR_SHIFT;
}

uint16_t GetSpritePatternTableBase(const register_file_t& registers)
{
    return (registers[6] & constants::VR6_SPRITE_PATTERN_MASK) << constants::VR6_SPRITE_PATTERN_SHIFT;
}

};

void copy_color(uint8_t* dst, uint8_t* src)
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

void set_color(uint8_t *color, uint8_t r, uint8_t g, uint8_t b)
{
    color[0] = r;
    color[1] = g;
    color[2] = b;
}

void nybble_to_color(uint8_t nybble, uint8_t color[3])
{
    static uint8_t nybbles_to_color[16][3] = {
        {0, 0, 0}, /* if BACKDROP is 0, supply black */
        {0, 0, 0},
        {37, 196, 37},
        {102, 226, 102},
        {37, 37, 226},
        {70, 102, 226},
        {165, 37, 37},
        {70, 196, 226},
        {226, 37, 37},
        {226, 102, 102},
        {196, 196, 37},
        {196, 196, 134},
        {37, 134, 37},
        {196, 70, 165},
        {165, 165, 165},
        {226, 226, 226},
    };

    copy_color(color, nybbles_to_color[nybble]);
}

template <size_t MEMORY_SIZE>
static void FillRowFromGraphicsI(int y, uint8_t row_colors[SCREEN_X], const TMS9918A::register_file_t& registers, const std::array<uint8_t, MEMORY_SIZE>& memory)
{
    using namespace TMS9918A;
    using namespace TMS9918A::constants;

    uint16_t row = y / 8;
    uint16_t pattern_row = y % 8;

    uint16_t name_row_base = GetNameTableBase(registers) | (row << ROW_SHIFT);
    uint16_t pattern_row_base = (registers[4] & VR4_PATTERN_MASK_STANDARD) << VR4_PATTERN_SHIFT_STANDARD | pattern_row;
    uint16_t color_base = (registers[3] & VR3_COLORTABLE_MASK_STANDARD) << VR3_COLORTABLE_SHIFT_STANDARD;

    uint8_t *rowp = row_colors;
    uint8_t backdrop = GetBackdropColor(registers);
    for(int x = 0; x < SCREEN_X; x += 8) {
        uint16_t col = x / 8;

        uint16_t name_table_address = name_row_base | col;
        uint8_t pattern_name = memory[name_table_address];
        uint16_t pattern_address = pattern_row_base | (pattern_name << CHARACTER_PATTERN_SHIFT);
        uint16_t color_address = color_base | (pattern_name >> CHARACTER_COLOR_SHIFT);

        uint8_t pattern_byte = memory[pattern_address];
        uint8_t colortable = memory[color_address];
        uint8_t color0 = colortable & 0xf;
        uint8_t color1 = (colortable >> 4) & 0xf;

        if(color0 == TRANSPARENT_COLOR_INDEX) {
            color0 = backdrop;
        }
        if(color1 == TRANSPARENT_COLOR_INDEX) {
            color1 = backdrop;
        }

        for(int pattern_col = 0; pattern_col < 8; pattern_col++) {
            bool bit = pattern_byte & (0x80 >> pattern_col);
            *rowp++ = bit ? color1 : color0;
        }
    }
}

template <size_t MEMORY_SIZE>
static void FillRowFromGraphicsII(int y, uint8_t row_colors[SCREEN_X], const TMS9918A::register_file_t& registers, const std::array<uint8_t, MEMORY_SIZE>& memory)
{
    using namespace TMS9918A;
    using namespace TMS9918A::constants;

    uint16_t row = y / 8;
    uint16_t pattern_row = y % 8;
    int third = (row / 8) << THIRD_SHIFT;

    uint16_t name_table_row_base = GetNameTableBase(registers) | (row << ROW_SHIFT);
    uint16_t address_mask = ((registers[3] & VR3_ADDRESS_MASK_BITMAP) << VR3_ADDRESS_MASK_SHIFT) | ADDRESS_MASK_FILL;
    uint16_t pattern_address_row_base = ((registers[4] & VR4_PATTERN_MASK_BITMAP) << VR4_PATTERN_SHIFT_BITMAP) | pattern_row | (third & address_mask);
    uint16_t color_address_row_base = ((registers[3] & VR3_COLORTABLE_MASK_BITMAP) << VR3_COLORTABLE_SHIFT_BITMAP) | pattern_row | (third & address_mask);

    uint8_t *rowp = row_colors;
    uint8_t backdrop = GetBackdropColor(registers);
    for(int x = 0; x < SCREEN_X; x += 8) {
        uint16_t col = x / 8;

        uint16_t name_table_address = name_table_row_base | col;
        uint16_t pattern_name = memory[name_table_address];

        uint16_t pattern_address = pattern_address_row_base | ((pattern_name << CHARACTER_PATTERN_SHIFT) & address_mask);
        uint16_t color_address = color_address_row_base | ((pattern_name << CHARACTER_PATTERN_SHIFT) & address_mask);
        uint8_t pattern_byte = memory[pattern_address];
        uint8_t colortable = memory[color_address];
        uint8_t color0 = colortable & 0xf;
        uint8_t color1 = (colortable >> 4) & 0xf;

        if(color0 == TRANSPARENT_COLOR_INDEX) {
            color0 = backdrop;
        }
        if(color1 == TRANSPARENT_COLOR_INDEX) {
            color1 = backdrop;
        }

        for(int pattern_col = 0; pattern_col < 8; pattern_col++) {
            bool bit = pattern_byte & (0x80 >> pattern_col);
            *rowp++ = bit ? color1 : color0;
        }
    }
}


template <size_t MEMORY_SIZE>
static void FillRowFromPattern(int y, uint8_t row_colors[SCREEN_X], const TMS9918A::register_file_t& registers, const std::array<uint8_t, MEMORY_SIZE>& memory)
{
    using namespace TMS9918A;
    using namespace TMS9918A::constants;

    GraphicsMode mode = GetGraphicsMode(registers);

    if(mode == GraphicsMode::GRAPHICS_I) {

        FillRowFromGraphicsI(y, row_colors, registers, memory);

    } else if(mode == GraphicsMode::GRAPHICS_II) {

        FillRowFromGraphicsII(y, row_colors, registers, memory);

    } else {

        bool M1 = registers[1] & VR1_M1_MASK;
        bool M2 = registers[1] & VR1_M2_MASK;
        bool M3 = registers[0] & VR0_M3_MASK;
        printf("unhandled video mode M1 = %d M2 = %d M3 = %d\n", M1, M2, M3);

        for(int x = 0; x < SCREEN_X; x++) {
            row_colors[x] = 8; // RED
        }
        // abort();
    }
}

template <size_t MEMORY_SIZE>
static uint8_t AddSpritesToRowReturnFlags(int row, uint8_t row_colors[SCREEN_X], const TMS9918A::register_file_t& registers, const std::array<uint8_t, MEMORY_SIZE>& memory)
{
    using namespace TMS9918A::constants;
    using namespace TMS9918A;

    bool sprite_touched[SCREEN_X]{};

    uint8_t flags_set = 0;

    // XXX do per row here because will do this per row on Rosa
    int sprite_table_address = GetSpriteAttributeTableBase(registers);
    bool mag2x = SpritesAreMagnified2X(registers);
    bool size4 = SpritesAreSize4(registers);
    int sprite_count = 32;
    for(int i = 0; i < 32; i++) {
        auto sprite = memory.begin() + sprite_table_address + i * 4;
        if(sprite[0] == 0xD0) {
            sprite_count = i;
            break;
        }
    }

    int size_pixels = 8;
    if(mag2x) {
        size_pixels *= 2;
    }

    if(size4) {
        size_pixels *= 2;
    }

    int sprites_in_row = 0;
    for(int i = sprite_count - 1; i >= 0; i--) {
        auto sprite = memory.begin() + sprite_table_address + i * 4;

        int sprite_y = sprite[0] + 1;
        int sprite_x = sprite[1];
        int sprite_name = sprite[2];
        bool sprite_earlyclock = sprite[3] & SPRITE_EARLY_CLOCK_MASK;
        int sprite_color = sprite[3] & SPRITE_COLOR_MASK;

        // printf("sprite %d: %d %d %d %d\n", i, sprite_x, sprite_y, sprite_name, sprite_color);

        if(sprite_earlyclock) {
            sprite_x -= 32;
        }

        int start_x = std::max(0, sprite_x);
        int start_y = std::max(0, sprite_y);
        int end_x = std::min(sprite_x + size_pixels, SCREEN_X) - 1;
        int end_y = std::min(sprite_y + size_pixels, SCREEN_Y) - 1;

        if(start_y <= row && row <= end_y) {

            int within_sprite_y = mag2x ? ((row - sprite_y) / 2) : (row - sprite_y);

            sprites_in_row ++;
            if(sprites_in_row > 5) {
                flags_set |= VDP_STATUS_5S_BIT;
            }

            for(int x = start_x; x <= end_x; x++) {

                int within_sprite_x = mag2x ? ((x - sprite_x) / 2) : (x - sprite_x);

                int bit = 0;

                if(size4) {

                    int quadrant = within_sprite_y / 8 + (within_sprite_x / 8) * 2;
                    int within_quadrant_y = within_sprite_y % 8;
                    int within_quadrant_x = within_sprite_x % 8;
                    int masked_sprite = sprite_name & SPRITE_NAME_MASK_SIZE4;
                    int sprite_pattern_address = GetSpritePatternTableBase(registers) | (masked_sprite << SPRITE_NAME_SHIFT) | (quadrant << 3) | within_quadrant_y;
                    bit = memory[sprite_pattern_address] & (0x80 >> within_quadrant_x);

                } else {

                    int sprite_pattern_address = GetSpritePatternTableBase(registers) | (sprite_name << SPRITE_NAME_SHIFT) | within_sprite_y;
                    bit = memory[sprite_pattern_address] & (0x80 >> within_sprite_x);
                }

                if(bit) {
                    if(sprite_touched[x]) {
                        flags_set |= VDP_STATUS_C_BIT;
                    }
                    sprite_touched[x] = true;
                    if(sprite_color != TRANSPARENT_COLOR_INDEX) {
                        row_colors[x] = sprite_color;
                    }
                }
            }
        }
    }
    return flags_set;
}

template <size_t MEMORY_SIZE, typename SetPixelFunc>
static uint8_t CreateImageAndReturnFlags(const TMS9918A::register_file_t& registers, const std::array<uint8_t, MEMORY_SIZE>& memory, SetPixelFunc SetPixel)
{
    using namespace TMS9918A::constants;
    using namespace TMS9918A;

    uint8_t flags_set = 0;

    if(ActiveDisplayAreaIsBlanked(registers)) {
        uint8_t color_index = GetBackdropColor(registers);
        uint8_t rgb[3];
        nybble_to_color(color_index, rgb);
        for(int row = 0; row < SCREEN_Y; row++) {
            for(int col = 0; col < SCREEN_X; col++) {
                SetPixel(col, row, rgb[0], rgb[1], rgb[2]);
            }
        }
        return flags_set;
    }

    static uint8_t row_colors[SCREEN_X];

    for(int row = 0; row < SCREEN_Y; row++) {

        FillRowFromPattern(row, row_colors, registers, memory);

        if(SpritesVisible(registers)) {
            flags_set |= AddSpritesToRowReturnFlags(row, row_colors, registers, memory);
        }

        for(int col = 0; col < SCREEN_X; col++) {
            uint8_t rgb[3];
            nybble_to_color(row_colors[col], rgb);
            SetPixel(col, row, rgb[0], rgb[1], rgb[2]);
        }
    }

    return flags_set;
}

template <size_t MEMORY_SIZE, typename SetPixelFunc>
static uint8_t CreateImageAndReturnFlags(const TMS9918A::register_file_t& registers, const std::array<uint8_t, MEMORY_SIZE>& memory, SetPixelFunc SetPixel);

struct TMS9918AEmulator
{
    bool cmd_started_in_nmi{false};
    int frame_number{0};
    int write_number{0};

    static constexpr int MEMORY_SIZE = 16384;
    std::array<uint8_t, MEMORY_SIZE> memory{};
    TMS9918A::register_file_t registers{};
    uint8_t status_register{0};

    enum {CMD_PHASE_FIRST, CMD_PHASE_SECOND} cmd_phase = CMD_PHASE_FIRST;
    unsigned char cmd_data = 0x0;
    unsigned int read_address = 0x0;
    unsigned int write_address = 0x0;

    TMS9918AEmulator()
    {
    }

    void vsync()
    {
        using namespace TMS9918A::constants;
        status_register |= VDP_STATUS_F_BIT;
    }


    void write(int cmd, unsigned char data)
    {
        using namespace TMS9918A::constants;
        if(debug & DEBUG_VDP_OPERATIONS) printf("VDP write %d cmd==%d, in_nmi = %d\n", write_number, cmd, z80state.in_nmi);
        if(do_save_images_on_vdp_write) { /* debug */

            static unsigned char framebuffer[SCREEN_X * SCREEN_Y * 4];
            auto pixel_setter = [](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                uint8_t *pixel = framebuffer + 4 * (x + y * SCREEN_X) + 0;
                set_color(pixel, r, g, b);
            };

            CreateImageAndReturnFlags(registers, memory, pixel_setter);
            char name[512];
            sprintf(name, "frame_%04d_%05d_%d_%02X.ppm", frame_number, write_number, cmd, data);
            FILE *fp = fopen(name, "wb");
            write_image(framebuffer, fp);
            fclose(fp);
        }
        write_number++;
        if(cmd) {

            if(cmd_phase == CMD_PHASE_FIRST) {

                if(debug & DEBUG_VDP_OPERATIONS) printf("VDP command write, first byte 0x%02X\n", data);
                cmd_data = data;
                cmd_phase = CMD_PHASE_SECOND;
                cmd_started_in_nmi = z80state.in_nmi;

            } else {

                if(z80state.in_nmi != cmd_started_in_nmi) {
                    if(cmd_started_in_nmi) {
                        printf("VDP cmd was started in NMI but finished outside NMI; likely corruption\n");
                    } else {
                        printf("VDP cmd was started outside NMI but finished inside NMI; likely corruption\n");
                    }
                    if(abort_on_exception) abort();
                }

                int cmd = data & CMD_MASK;
                if(cmd == CMD_SET_REGISTER) {
                    int which_register = data & REG_A0_A5_MASK;
                    if(debug & DEBUG_VDP_OPERATIONS) printf("VDP command write to register 0x%02X, value 0x%02X\n", which_register, cmd_data);
                    registers[which_register] = cmd_data;
                } else if(cmd == CMD_SET_WRITE_ADDRESS) {
                    write_address = ((data & REG_A0_A5_MASK) << 8) | cmd_data;
                    if(debug & DEBUG_VDP_OPERATIONS) printf("VDP write address set to 0x%04X\n", write_address);
                } else if(cmd == CMD_SET_READ_ADDRESS) {
                    read_address = ((data & REG_A0_A5_MASK) << 8) | cmd_data;
                    if(debug & DEBUG_VDP_OPERATIONS) printf("VDP read address set to 0x%04X\n", write_address);
                } else {
                    if((debug & DEBUG_VDP_OPERATIONS) && !abort_on_exception) printf("VDP cmd was unknown 0x%02X!\n", cmd);
                    if(abort_on_exception) {
                        printf("VDP cmd was unknown 0x%02X, aborting\n", cmd);
                        abort();
                    }
                }
                cmd_phase = CMD_PHASE_FIRST;
            }

        } else {

            if(debug & DEBUG_VDP_OPERATIONS) {
                static char bitfield[9];
                for(int i = 0; i < 8; i++) bitfield[i] = (data & (0x80 >> i)) ? '*' : ' ';
                bitfield[8] = '\0';
                if(isprint(data)) {
                    printf("VDP data write 0x%02X, '%s' ('%c')\n", data, bitfield, data);
                } else {
                    printf("VDP data write 0x%02X, '%s'\n", data, bitfield);
                }
            }
            memory[write_address++] = data;
            write_address = write_address % MEMORY_SIZE;
        }

    }

    uint8_t read(int cmd)
    {
        using namespace TMS9918A::constants;
        if(cmd) {
            if(cmd_phase == CMD_PHASE_SECOND) {
                if(z80state.in_nmi) {
                    printf("cmd_phase was reset in ISR\n");
                } else {
                    printf("cmd_phase was reset outside ISR\n");
                }
                abort();
            }
            cmd_phase = CMD_PHASE_FIRST;
            uint8_t data = status_register;
            status_register = 0;  
            return data;
        } else {
            uint8_t data = memory[read_address++];
            read_address = read_address % MEMORY_SIZE;
            return data;
        }
    }

    void perform_scanout(unsigned char image[SCREEN_X * SCREEN_Y * 4])
    {
        using namespace TMS9918A::constants;
        frame_number++;
        write_number = 0;
        if(debug & DEBUG_SCANOUT) {
            printf("scanout frame %d\n", frame_number);
        }

        auto pixel_setter = [&image](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
            uint8_t *pixel = image + 4 * (x + y * SCREEN_X) + 0;
            set_color(pixel, r, g, b);
        };

        status_register |= CreateImageAndReturnFlags(registers, memory, pixel_setter);
    }

    bool nmi_required()
    {
        using namespace TMS9918A;
        return InterruptsAreEnabled(registers) && VSyncInterruptHasOccurred(status_register);
    }
};


struct ColecoHW : board_base
{
    TMS9918AEmulator vdp;
    SN76489A sound;

    bool reading_joystick = true;

    static constexpr int VDP_DATA_PORT = 0xBE;
    static constexpr int VDP_CMD_PORT = 0xBF;

    static constexpr int SN76489A_PORT = 0xFF;

    static constexpr int SWITCH_TO_KEYPAD_PORT = 0x80;
    static constexpr int SWITCH_TO_JOYSTICK_PORT = 0xC0;
    static constexpr int CONTROLLER1_PORT = 0xFC;
    static constexpr int CONTROLLER2_PORT = 0xFF;

    ColecoHW(int sample_rate, size_t audio_buffer_size) :
        sound(machine_clock_rate, sample_rate, audio_buffer_size)
    {
    }

    virtual bool io_write(int addr, unsigned char data)
    {
        // if(addr == ColecoHW::PROPELLER_PORT) {
            // write_to_propeller(data);
            // return true;
        // }

        if(false) {
            if(addr == ColecoHW::VDP_CMD_PORT) {
                vdp.write(1, data);
#ifdef PROVIDE_DEBUGGER
                io_writes.insert({addr, data});
#endif
                return true;
            }

            if(addr == ColecoHW::VDP_DATA_PORT) {
                vdp.write(0, data);
#ifdef PROVIDE_DEBUGGER
                io_writes.insert({addr, data});
#endif
                return true;
            }
        } else {
            if((addr >= 0xA0) && (addr <= 0xBF)) {
                vdp.write(addr & 0x1, data);
#ifdef PROVIDE_DEBUGGER
                io_writes.insert({addr, data});
#endif
                return true;
            }
        }

        /* if(addr == ColecoHW::SN76489A_PORT) { */
        if((addr >= 0xE0) && (addr <= 0xFF)) {
            if(debug & DEBUG_IO) printf("audio write 0x%02X\n", data);
            sound.write(data);
#ifdef PROVIDE_DEBUGGER
            io_writes.insert({addr, data});
#endif
            return true;
        }

        if(addr == ColecoHW::SWITCH_TO_KEYPAD_PORT) {
            if(debug & DEBUG_IO) printf("switch to keypad\n");
            reading_joystick = false;
#ifdef PROVIDE_DEBUGGER
            io_writes.insert({addr, data});
#endif
            return true;
        }

        if(addr == ColecoHW::SWITCH_TO_JOYSTICK_PORT) {
            if(debug & DEBUG_IO) printf("switch to keypad\n");
            reading_joystick = true;
#ifdef PROVIDE_DEBUGGER
            io_writes.insert({addr, data});
#endif
            return true;
        }

        return false;
    }

    virtual bool io_read(int addr, unsigned char &data)
    {
        if(false) {
            if(addr == ColecoHW::VDP_CMD_PORT) {
                if(debug & DEBUG_IO) printf("read VDP command port\n");
                data = vdp.read(1);
#ifdef PROVIDE_DEBUGGER
                io_reads.insert(addr);
#endif
                return true;
            }

            if(addr == ColecoHW::VDP_DATA_PORT) {
                if(debug & DEBUG_IO) printf("read VDP command port\n");
                data = vdp.read(0);
#ifdef PROVIDE_DEBUGGER
                io_reads.insert(addr);
#endif
                return true;
            }
        } else {
            if((addr >= 0xA0) && (addr <= 0xBF)) {
                if(debug & DEBUG_IO) printf("read VDP 0x%02X\n", addr);
                data = vdp.read(addr & 0x1);
#ifdef PROVIDE_DEBUGGER
                io_reads.insert(addr);
#endif
                return true;
            }
        }

        if((addr >= 0xE0) && (addr <= 0xFF) && ((addr & 0x02) == 0x0)) {
            if(reading_joystick) {
                data = COLECOinterface::GetJoystickState(COLECOinterface::CONTROLLER_1);
            } else {
                data = COLECOinterface::GetKeypadState(COLECOinterface::CONTROLLER_1);
            }
            if(debug & DEBUG_IO) printf("read controller1 port 0x%02X, read 0x%02X\n", addr, data);
#ifdef PROVIDE_DEBUGGER
            io_reads.insert(addr);
#endif
            return true;
        }

        if((addr >= 0xE0) && (addr <= 0xFF) && ((addr & 0x02) == 0x2)) {
            if(reading_joystick) {
                data = COLECOinterface::GetJoystickState(COLECOinterface::CONTROLLER_2);
            } else {
                data = COLECOinterface::GetKeypadState(COLECOinterface::CONTROLLER_2);
            }
            if(debug & DEBUG_IO) printf("read controller2 port 0x%02X, read 0x%02X\n", addr, data);
#ifdef PROVIDE_DEBUGGER
            io_reads.insert(addr);
#endif
            return true;
        }

        if(debug & DEBUG_IO) printf("read unknown address 0x%02X\n", addr);

        if(break_on_unknown_address) {
            abort();
        }

        return false;
    }

    virtual void init(void)
    {
    }
    virtual void idle(void)
    {
    }
    virtual void pause(void) {};
    virtual void resume(void) {};

    virtual bool nmi_required()
    {
        return vdp.nmi_required();
    }

    void fill_flush_audio(clk_t clk, audio_flush_func audio_flush)
    {
        sound.generate_audio(clk, audio_flush);
    }
};

struct RAMboard : board_base
{
    int base;
    int length;
    std::unique_ptr<unsigned char> bytes;
    RAMboard(int base_, int length_) :
        base(base_),
        length(length_),
        bytes(new unsigned char[length])
    {
    }
    virtual bool memory_read(int addr, unsigned char &data)
    {
        if(addr >= base && addr < base + length) {
            data = bytes.get()[addr - base];
            if(debug & DEBUG_RAM) printf("read 0x%04X -> 0x%02X from RAM\n", addr, data);
            return true;
        }
        return false;
    }
    virtual bool memory_write(int addr, unsigned char data)
    {
        if(addr >= base && addr < base + length) {
            bytes.get()[addr - base] = data;
            if(debug & DEBUG_RAM) printf("wrote 0x%02X to RAM 0x%04X\n", data, addr);
            return true;
        }
        return false;
    }
};

struct ROMboard : board_base
{
    int base;
    int length;
    std::unique_ptr<unsigned char> bytes;
    ROMboard(int base_, int length_, unsigned char bytes_[]) : 
        base(base_),
        length(length_),
        bytes(new unsigned char[length])
    {
        memcpy(bytes.get(), bytes_, length);
    }
    virtual bool memory_read(int addr, unsigned char &data)
    {
        if(addr >= base && addr < base + length) {
            data = bytes.get()[addr - base];
            if(debug & DEBUG_ROM) printf("read 0x%04X -> 0x%02X from ROM\n", addr, data);
            return true;
        }
        return false;
    }
    virtual bool memory_write(int addr, unsigned char data)
    {
        if(addr >= base && addr < base + length) {
            if(debug & DEBUG_ROM) printf("attempted write 0x%02X to ROM 0x%04X ignored\n", data, addr);
        }
        return false;
    }
};

void print_state(Z80_STATE* state)
{
    printf("BC :%04X  DE :%04X  HL :%04X  AF :%04X  IX : %04X  IY :%04X  SP :%04X\n",
        state->registers.word[Z80_BC], state->registers.word[Z80_DE],
        state->registers.word[Z80_HL], state->registers.word[Z80_AF],
        state->registers.word[Z80_IX], state->registers.word[Z80_IY],
        state->registers.word[Z80_SP]);
    printf("BC':%04X  DE':%04X  HL':%04X  AF':%04X\n",
        state->alternates[Z80_BC], state->alternates[Z80_DE],
        state->alternates[Z80_HL], state->alternates[Z80_AF]);
    printf("PC :%04X\n",
        state->pc);
}

#ifdef PROVIDE_DEBUGGER

struct BreakPoint
{
    enum Type {INSTRUCTION, DATA} type;
    int address;
    unsigned char old_value;
    bool enabled;
    BreakPoint(int address_, unsigned char old_value_) :
        type(DATA),
        address(address_),
        old_value(old_value_),
        enabled(true)
    {
    }
    BreakPoint(int address_) :
        type(INSTRUCTION),
        address(address_),
        old_value(0),
        enabled(true)
    {
    }
    void enable() { enabled = true; }
    void disable() { enabled = false; }
};

void clear_breakpoints(std::vector<BreakPoint>& breakpoints, Z80_STATE* state)
{
    for(auto i = breakpoints.begin(); i != breakpoints.end(); i++) {
        BreakPoint& bp = (*i);
        switch(bp.type) {
            case BreakPoint::DATA:
                Z80_READ_BYTE(bp.address, bp.old_value);
                break;
            case BreakPoint::INSTRUCTION:
                break;
        }
    }
}

bool is_breakpoint_triggered(std::vector<BreakPoint>& breakpoints, Z80_STATE* state, int& which)
{
    for(auto i = breakpoints.begin(); i != breakpoints.end(); i++) {
        BreakPoint& bp = (*i);
        if(bp.enabled)
            switch(bp.type) {
                case BreakPoint::INSTRUCTION:
                    if(state->pc == bp.address) {
                        which = i - breakpoints.begin();
                        return true;
                    }
                    break;
                case BreakPoint::DATA:
                    unsigned char data;
                    Z80_READ_BYTE(bp.address, data);
                    if(data != bp.old_value) {
                        which = i - breakpoints.begin();
                        return true;
                    }
                    break;
            }
    }
    return false;
}

struct Debugger
{
    ColecoHW* colecohw{nullptr};
    clk_t& clk;
    std::vector<BreakPoint> breakpoints;
    std::set<int> io_watch;
    std::string address_to_symbol[65536]; // XXX excessive memory?
    std::map<std::string, int> symbol_to_address; // XXX excessive memory?
    sig_t previous_sigint;
    bool state_may_have_changed;
    bool last_was_step;
    bool last_was_jump;
    std::string& get_symbol(int address, int& offset)
    {
        static std::string no_symbol = "";
        offset = 0;
        while(address >= 0 && address_to_symbol[address].empty()) {
            address--;
            offset++;
        }
        if(address < 0) {
            return no_symbol;
        }
        return address_to_symbol[address];
    }
    bool load_symbols(char *filename)
    {
        FILE *fp = fopen(filename, "ra");
        if(fp == NULL) {
            fprintf(stderr, "couldn't open %s to read symbols\n", filename);
            return false;
        }
        fseek(fp, 0, SEEK_END);
        ssize_t size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char* buffer = new char[size];
        fread(buffer, size, 1, fp);
        fclose(fp);
        char *symbol_part = buffer;
        while((symbol_part - buffer) < size && (*symbol_part != '')) {
            symbol_part++;
        }
        if(symbol_part - buffer >= size) {
            fprintf(stderr, "couldn't find symbol section in %s\n", filename);
            delete[] buffer;
            return false;
        }
        int address, consumed;
        char symbol[512];
        while(sscanf(symbol_part, "%x %s%n", &address, symbol, &consumed) == 2) {
            address_to_symbol[address] = symbol;
            symbol_to_address[symbol] = address;
            symbol_part += consumed;
        }

        delete[] buffer;
        return true;
    }
    void ctor()
    {
        state_may_have_changed = true;
        last_was_step = false;
        last_was_jump = false;
    }
    Debugger(ColecoHW *colecohw, clk_t& clk) :
        colecohw(colecohw),
        clk(clk)
    {
        ctor();
    }
    bool process_line(std::vector<board_base*>& boards, Z80_STATE* state, char *line);
    bool process_command(std::vector<board_base*>& boards, Z80_STATE* state, char *command);
    void go(FILE *fp, std::vector<board_base*>& boards, Z80_STATE* state);
    bool should_debug(std::vector<board_base*>& boards, Z80_STATE* state);
};

#include "bg80d.h"

__uint8_t reader(void *p)
{
    int& address = *(int*)p;
    unsigned char data;
    Z80_READ_BYTE(address, data);
    address++;
    return data;
}

int disassemble(int address, Debugger *d, int bytecount)
{
    int total_bytes = 0;

#if USE_BG80D

    while(bytecount > 0) {

        int address_was = address;

        int symbol_offset;
        std::string& sym = d->get_symbol(address, symbol_offset);

        bg80d::opcode_spec_t *opcode = bg80d::decode(reader, &address, address);
        if(opcode == 0) {
            break;
        }

        printf("%04X %s+0x%04X%*s", address_was, sym.c_str(), symbol_offset, 16 - (int)sym.size() - 5, "");

        int opcode_length = (opcode->pc_after - address_was);
        int opcode_bytes_pad = 1 + 3 + 3 + 3 - opcode_length * 3;
        for(int i = 0; i < opcode_length; i++) {
            unsigned char byte;
            Z80_READ_BYTE(address_was + i, byte);
            printf("%.2hhX ", byte);
        }

        printf("%*s", opcode_bytes_pad, "");
        printf("%5s %s\n", opcode->prefix, opcode->description);

        bytecount -= opcode_length;
	total_bytes += opcode_length;
    }
    // FB5C conin+0x0002         e6     AND A, n        ff          ;  AND of ff to reg

#else

    int symbol_offset;
    std::string& sym = d->get_symbol(address, symbol_offset);
    printf("%04X %s+0x%04X%*s : %02X %02X %02X\n", address, sym.c_str(), symbol_offset, 16 - (int)sym.size() - 5, "", buffer[0], buffer[1], buffer[2]);

#endif

    return total_bytes;
}

void disassemble_instructions(int address, Debugger *d, int insncount)
{
    for(int i = 0; i < insncount; i++) {
	address += disassemble(address, d, 1);
    }
}


// XXX make this pointers-to-members
typedef bool (*command_handler)(Debugger* d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv);

std::map<std::string, command_handler> command_handlers;

bool lookup_or_parse(std::map<std::string, int>& symbol_to_address, char *s, int& a)
{
    auto found = symbol_to_address.find(s);
    if(found != symbol_to_address.end()) {
        a = found->second;
    } else {
        char *endptr;
        a = strtol(s, &endptr, 0);
        if(*endptr != '\0') {
            printf("number parsing failed for %s; forgot to lead with 0x?\n", s);
            return false;
        }
    }
    return true;
}

void store_memory(void *arg, int address, unsigned char p)
{
    int *info = (int*)arg;
    Z80_WRITE_BYTE(address, p);
    info[0] = std::min(info[0], address);
    info[1] = std::max(info[1], address);
    info[2]++; // XXX Could be overwrites...
}

bool debugger_readhex(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "readhex: expected filename argument\n");
        return false;
    }
    FILE *fp = fopen(argv[1], "ra");
    if(fp == NULL) {
        fprintf(stderr, "failed to open %s for reading\n", argv[1]);
        return false;
    }
    int info[3] = {0xffff, 0, 0};
    int success = read_hex(fp, store_memory, info, 0);
    if (!success) {
        fprintf(stderr, "error reading hex file %s\n", argv[1]);
        fclose(fp);
        return false;
    }
    printf("Read %d (0x%04X) bytes from %s into 0x%04X..0x%04X (might be sparse)\n",
        info[2], info[2], argv[1], info[0], info[1]);
    fclose(fp);
    return false;
}

bool debugger_readbin(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 3) {
        fprintf(stderr, "readbin: expected filename and address\n");
        return false;
    }
    static unsigned char buffer[128];

    int address;
    if(!lookup_or_parse(d->symbol_to_address, argv[2], address))
        return false;

    int a = address;
    FILE *fp = fopen(argv[1], "rb");
    if(fp == NULL) {
        fprintf(stderr, "failed to open %s for reading\n", argv[1]);
        return false;
    }
    size_t size;
    while((size = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        for(size_t i = 0; i <= size; i++, a++)
            Z80_WRITE_BYTE(a, buffer[i]);
    }
    printf("Read %d (0x%04X) bytes from %s into 0x%04X..0x%04X\n",
        a - address, a - address, argv[1], address, a - 1);
    fclose(fp);
    return false;
}

void dump_buffer_hex(int indent, int actual_address, unsigned char *data, int size)
{
    int address = 0;
    int screen_lines = 0;

    while(size > 0) {
        if(screen_lines >= 24) { 
            printf(":");
            static char line[512];
            fgets(line, sizeof(line), stdin);
            if(strcmp(line, "q") == 0)
                return;
            screen_lines = 0;
        }
        int howmany = std::min(size, 16);

        printf("%*s0x%04X: ", indent, "", actual_address + address);
        for(int i = 0; i < howmany; i++)
            printf("%02X ", data[i]);
        printf("\n");

        printf("%*s        ", indent, "");
        for(int i = 0; i < howmany; i++)
            printf(" %c ", isprint(data[i]) ? data[i] : '.');
        printf("\n");
        screen_lines++;

        size -= howmany;
        data += howmany;
        address += howmany;
    }
}

bool debugger_dis(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 3) {
        fprintf(stderr, "dis: expected address and count\n");
        return false;
    }
    char *endptr;

    int address;
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address)) {
        return false;
    }

    int count = strtol(argv[2], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[2]);
        return false;
    }
    disassemble_instructions(address, d, count);
    return false;
}

bool debugger_dump(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 3) {
        fprintf(stderr, "dump: expected address and length\n");
        return false;
    }
    char *endptr;

    int address;
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address)) {
        return false;
    }

    int length = strtol(argv[2], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[2]);
        return false;
    }
    static unsigned char buffer[65536];
    for(int i = 0; i < length; i++) {
        Z80_READ_BYTE(address + i, buffer[i]);
    }
    dump_buffer_hex(4, address, buffer, length);
    return false;
}

bool debugger_symbols(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "symbols: expected filename argument\n");
        return false;
    }
    d->load_symbols(argv[1]);
    return false;
}

bool debugger_fill(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 4) {
        fprintf(stderr, "fill: expected address, length, and value\n");
        return false;
    }
    char *endptr;

    int address;
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address)) {
        return false;
    }

    int length = strtol(argv[2], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[2]);
        return false;
    }
    int value = strtol(argv[3], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[3]);
        return false;
    }
    printf("fill %d for %d with %d\n", address, length, value);
    for(int i = 0; i < length; i++) {
        Z80_WRITE_BYTE(address + i, value);
    }
    return false;
}

bool debugger_image(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    FILE *fp = fopen("output.ppm", "wb");

    auto& vdp = d->colecohw->vdp;

    // XXX
    static unsigned char framebuffer[SCREEN_X * SCREEN_Y * 4];
    auto pixel_setter = [](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t *pixel = framebuffer + 4 * (x + y * SCREEN_X) + 0;
        set_color(pixel, r, g, b);
    };
    std::chrono::time_point<std::chrono::system_clock> start_time = std::chrono::system_clock::now();
    CreateImageAndReturnFlags(vdp.registers, vdp.memory, pixel_setter);
    std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed = now - start_time;
    if(false) printf("dump time %f seconds\n", elapsed.count());
    write_image(framebuffer, fp);
    fclose(fp);

    fp = fopen("vdp_memory.txt", "w");
    fprintf(fp, "%02X %02X %02X %02X %02X %02X %02X %02X\n",
        vdp.registers[0], vdp.registers[1], vdp.registers[2], vdp.registers[3],
        vdp.registers[4], vdp.registers[5], vdp.registers[6], vdp.registers[7]);
    for(int j = 0; j < 64; j++) {
        for(int i = 0; i < 256; i++) {
            fprintf(fp, "%02X ", vdp.memory[j * 256 + i]);
        }
        fprintf(fp, "\n");
    }
    fclose(fp);

    return false;
}

bool debugger_in(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "out: expected port number\n");
        return false;
    }
    char *endptr;
    int port = strtol(argv[1], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[1]);
        return false;
    }
    unsigned char byte;
    Z80_INPUT_BYTE(port, byte);
    printf("received byte 0x%02X from port %d (0x%02X)\n", byte, port, port);
    return false;
}

bool debugger_out(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 3) {
        fprintf(stderr, "out: expected port number and byte\n");
        return false;
    }
    char *endptr;
    int port = strtol(argv[1], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[1]);
        return false;
    }
    int value = strtol(argv[2], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[2]);
        return false;
    }
    Z80_OUTPUT_BYTE(port, value);
    return false;
}

bool debugger_help(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    printf("Debugger commands:\n");
    printf("    go                    - continue normally\n");
    printf("    dump addr count       - dump count bytes at addr\n");
    printf("    fill addr count byte  - fill count bytes with byte at addr\n");
    printf("    readhex file.hex      - read file.hex into memory\n");
    printf("    readbin addr file.bin - read file.bin into memory at addr\n");
    printf("    symbols file.prn      - read symbols from file\n");
    printf("    step [N]              - step [for N instructions]\n");
    printf("    watch addr            - break out of step if addr changes\n");
    printf("    watchio addr          - break out of step if addr is IO read or write\n");
    printf("    break addr            - break into debugger at addr\n");
    printf("    disable N             - disable breakpoint N\n");
    printf("    enable N              - disable breakpoint N\n");
    printf("    remove N              - remove breakpoint N\n");
    printf("    list                  - list breakpoints and catchpoints\n");
    printf("    jump addr             - jump to addr \n");
    printf("    pc addr               - set PC to addr (in anticipation of \"step\")\n");
    printf("    in port               - input byte from port and print it\n");
    printf("    out port byte         - output byte to port\n");
    printf("    help                  - print this help message\n");
    printf("    ?                     - print this help message\n");
    printf("    dis addr count        - disassemble count instructions at addr\n");
    printf("    quit, exit, ^D        - exit the debugger\n");
    return false;
}

bool debugger_continue(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    d->state_may_have_changed = true;
    return true;
}

bool brads_zero_check = true;

bool debugger_step(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    int count = 1;
    bool verbose = false;

    if((argc > 1) && (strcmp(argv[1], "-v") == 0)) {
        verbose = true;
        argc--;
        argv++;
    }
    if(argc > 1) {
        char *endptr;
        count = strtol(argv[1], &endptr, 0);
        if(*endptr != '\0') {
            printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[1]);
            return false;
        }
    }
    for(int i = 0; i < count; i++) {
        d->clk += Z80Emulate(state, 1);
        if(i < count - 1) {
            if(verbose) {
                print_state(state);
                disassemble(state->pc, d, 1);
            }
        }
        if(d->should_debug(boards, state)) {
            break;
        }
    }
    printf("%llu actual cycles emulated\n", d->clk);
    d->state_may_have_changed = true;
    d->last_was_step = true;
    return false;
}

bool debugger_jump(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "jump: expected address\n");
        return false;
    }

    if(!lookup_or_parse(d->symbol_to_address, argv[1], state->pc)) {
        return false;
    }

    char *endptr;
    state->pc = strtol(argv[1], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[1]);
        return false;
    }

    d->state_may_have_changed = true;
    d->last_was_jump = true;
    return true;
}

bool debugger_pc(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "jump: expected address\n");
        return false;
    }
    char *endptr;
    state->pc = strtol(argv[1], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[1]);
        return false;
    }
    d->state_may_have_changed = true;
    return false;
}

bool debugger_quit(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    quit = true;
    return true;
}

bool debugger_break(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "break: expected address\n");
        return false;
    }

    int address;
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address)) {
        return false;
    }

    d->breakpoints.push_back(BreakPoint(address));
    return false;
}

bool debugger_watch(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "watch: expected address\n");
        return false;
    }

    int address;
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address)) {
        return false;
    }

    unsigned char old_value;
    Z80_READ_BYTE(address, old_value);
    d->breakpoints.push_back(BreakPoint(address, old_value));
    return false;
}

bool debugger_watchio(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "watchio: expected address\n");
        return false;
    }

    int address;
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address)) {
        return false;
    }

    if(d->io_watch.count(address) > 0) {
        fprintf(stderr, "watchio: removing watch on 0x%X\n", address);
        d->io_watch.erase(address);
    } else {
        fprintf(stderr, "watchio: adding watch on 0x%X\n", address);
        d->io_watch.insert(address);
    }
    return false;
}

bool debugger_disable(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "break: expected address\n");
        return false;
    }
    char *endptr;
    int i = strtol(argv[1], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[1]);
        return false;
    }
    if(i < 0 || (size_t)i >= d->breakpoints.size()) {
        fprintf(stderr, "breakpoint %d is out of range\n", i);
        return false;
    }
    d->breakpoints[i].disable();
    return false;
}

bool debugger_enable(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "break: expected address\n");
        return false;
    }
    char *endptr;
    int i = strtol(argv[1], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[1]);
        return false;
    }
    if(i < 0 || (size_t)i >= d->breakpoints.size()) {
        fprintf(stderr, "breakpoint %d is out of range\n", i);
        return false;
    }
    d->breakpoints[i].enable();
    return false;
}

bool debugger_remove(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "break: expected address\n");
        return false;
    }
    char *endptr;
    int i = strtol(argv[1], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[1]);
        return false;
    }
    if(i < 0 || (size_t)i >= d->breakpoints.size()) {
        fprintf(stderr, "breakpoint %d is out of range\n", i);
        return false;
    }
    d->breakpoints.erase(d->breakpoints.begin() + i);
    return false;
}

bool debugger_list(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    printf("breakpoints:\n");
    for(auto i = d->breakpoints.begin(); i != d->breakpoints.end(); i++) {
        BreakPoint& bp = (*i);
        printf("%ld : ", i - d->breakpoints.begin());
        printf("%s ", bp.enabled ? " enabled" : "disabled");
        printf("%s ", (bp.type == BreakPoint::INSTRUCTION) ? " ins" : "data");
        if(bp.type == BreakPoint::INSTRUCTION) {
            int symbol_offset;
            std::string& sym = d->get_symbol(bp.address, symbol_offset);
            printf("break at 0x%04x (%s+%d)\n", bp.address, sym.c_str(), symbol_offset);
        } else {
            printf("change at 0x%04X from 0x%02X\n", bp.address, bp.old_value);
        }
    }
    return false;
}

void populate_command_handlers()
{
    command_handlers["image"] = debugger_image;
    command_handlers["?"] = debugger_help;
    command_handlers["help"] = debugger_help;
    command_handlers["readhex"] = debugger_readhex;
    command_handlers["readbin"] = debugger_readbin;
    command_handlers["dump"] = debugger_dump;
    command_handlers["fill"] = debugger_fill;
    command_handlers["symbols"] = debugger_symbols;
    command_handlers["in"] = debugger_in;
    command_handlers["out"] = debugger_out;
    command_handlers["go"] = debugger_continue;
    command_handlers["g"] = debugger_continue;
    command_handlers["step"] = debugger_step;
    command_handlers["jump"] = debugger_jump;
    command_handlers["pc"] = debugger_pc;
    command_handlers["break"] = debugger_break;
    command_handlers["watch"] = debugger_watch;
    command_handlers["watchio"] = debugger_watchio;
    command_handlers["enable"] = debugger_enable;
    command_handlers["disable"] = debugger_disable;
    command_handlers["remove"] = debugger_remove;
    command_handlers["list"] = debugger_list;
    command_handlers["quit"] = debugger_quit;
    command_handlers["exit"] = debugger_quit;
    command_handlers["dis"] = debugger_dis;
        // reset
}

bool Debugger::process_command(std::vector<board_base*>& boards, Z80_STATE* state, char *command)
{
    // process commands
    char **ap, *argv[10];

    for (ap = argv; (*ap = strsep(&command, " \t")) != NULL;) {
        if (**ap != '\0') {
            if (++ap >= &argv[10]) {
                break;
            }
        }
    }
    int argc = ap - argv;

    if(argc == 0) {
        if(last_was_step) {
            return debugger_step(this, boards, state, argc, argv);
        } else {
            return false;
        }
    }

    last_was_step = false;
    auto it = command_handlers.find(argv[0]);
    if(it == command_handlers.end()) {
        fprintf(stderr, "debugger command not defined: \"%s\"\n", argv[0]);
        return false;
    }
    
    return (*it).second(this, boards, state, argc, argv);
}

bool Debugger::process_line(std::vector<board_base*>& boards, Z80_STATE* state, char *line)
{
    char *command;

    while((command = strsep(&line, ";")) != NULL) {
        bool run = process_command(boards, state, command);
        if(run) {
            return true;
        }
    }
    return false;
}

bool Debugger::should_debug(std::vector<board_base*>& boards, Z80_STATE* state)
{
    int which;
    for(auto *b : boards) {
        auto io_reads_tmp = b->io_reads;
        auto io_writes_tmp = b->io_writes;
        b->io_reads.clear();
        b->io_writes.clear();
        for(auto io_read: io_reads_tmp) {
            if(io_watch.count(io_read) > 0) {
                return true;
            }
        }
        for(auto [io_write_address, io_write_value]: io_writes_tmp) {
            if(io_watch.count(io_write_address) > 0) {
                return true;
            }
        }
    }
    bool should = !last_was_jump && is_breakpoint_triggered(breakpoints, state, which);
    last_was_jump = false;
    return should;
}

bool enter_debugger = false;

void mark_enter_debugger(int signal)
{
    enter_debugger = true;
}

void Debugger::go(FILE *fp, std::vector<board_base*>& boards, Z80_STATE* state)
{
    signal(SIGINT, previous_sigint);
    for(auto b = boards.begin(); b != boards.end(); b++) {
        (*b)->pause();
    }

    if(!feof(fp)) {
        bool run = false;
        do {
            if(state_may_have_changed) {
                state_may_have_changed = false;
                print_state(state);
                disassemble(state->pc, this, 1);
            }
            int which;
            if(is_breakpoint_triggered(breakpoints, state, which))
            {
                printf("breakpoint %d: ", which);
                BreakPoint& bp = breakpoints[which];
                if(bp.type == BreakPoint::INSTRUCTION) {
                    int symbol_offset;
                    std::string& sym = get_symbol(state->pc, symbol_offset);
                    printf("break at 0x%04x (%s+%d)\n", bp.address, sym.c_str(), symbol_offset);
                } else {
                    unsigned char new_value;
                    Z80_READ_BYTE(bp.address, new_value);
                    printf("change at 0x%04X from 0x%02X to 0x%02X\n", bp.address, bp.old_value, new_value);
                }
                clear_breakpoints(breakpoints, state);
            }
            if(fp == stdin) {
                char *line;
                line = readline("? ");
                if (line == NULL) {
                    printf("\n");
                    quit = true;
                    run = true;
                } else {
                    if(strlen(line) > 0) {
                        add_history(line);
                    }
                    run = process_line(boards, state, line);
                    free(line);
                }
            } else {
                char line[512];
                if(fgets(line, sizeof(line), fp) == NULL) {
                    break;
                }
                line[strlen(line) - 1] = '\0';
                run = process_line(boards, state, line);
            }
            for(auto b = boards.begin(); b != boards.end(); b++)  {
                (*b)->idle();
            }

        } while(!run);
    }

    for(auto b = boards.begin(); b != boards.end(); b++)  {
        (*b)->resume();
    }

    previous_sigint = signal(SIGINT, mark_enter_debugger);
    state_may_have_changed = true;
}

#endif

void usage(char *progname)
{
    printf("\n");
    printf("usage: %s [options] bios.bin cartridge.bin\n", progname);
    printf("\n");
    printf("options:\n");
    printf("\t-debugger init          Invoke debugger on startup\n");
    printf("\t                        \"init\" can be commands (separated by \";\"\n");
    printf("\t                        or a filename.  The initial commands can be\n");
    printf("\t                        the empty string.\n");
    printf("\n");
}

using namespace COLECOinterface;

static GLFWwindow* my_window;

GLuint image_program;
GLuint image_texture_location;
GLuint image_texture_coord_scale_location;
GLuint image_to_screen_location;
GLuint image_x_offset_location;
GLuint image_y_offset_location;

const int raster_coords_attrib = 0;

bool use_joystick = false;
int joystick_button_west = 1;
int joystick_button_east = 1;
int joystick_button_north = 1;
int joystick_button_south = 1;
int joystick_button_fire_left = 1;
int joystick_button_fire_right = 1;

static int gWindowWidth, gWindowHeight;

// to handle https://github.com/glfw/glfw/issues/161
static double gMotionReported = false;

static double gOldMouseX, gOldMouseY;
static int gButtonPressed = -1;

float pixel_to_ui_scale;
float to_screen_transform[9];

void make_to_screen_transform()
{
    to_screen_transform[0 * 3 + 0] = 2.0 / gWindowWidth * pixel_to_ui_scale;
    to_screen_transform[0 * 3 + 1] = 0;
    to_screen_transform[0 * 3 + 2] = 0;
    to_screen_transform[1 * 3 + 0] = 0;
    to_screen_transform[1 * 3 + 1] = -2.0 / gWindowHeight * pixel_to_ui_scale;
    to_screen_transform[1 * 3 + 2] = 0;
    to_screen_transform[2 * 3 + 0] = -1;
    to_screen_transform[2 * 3 + 1] = 1;
    to_screen_transform[2 * 3 + 2] = 1;
}

opengl_texture screen_image;
vertex_array screen_image_rectangle;

static const char *hires_vertex_shader = "\n\
    uniform mat3 to_screen;\n\
    attribute vec2 vertex_coords;\n\
    varying vec2 raster_coords;\n\
    uniform float x_offset;\n\
    uniform float y_offset;\n\
    \n\
    void main()\n\
    {\n\
        raster_coords = vertex_coords;\n\
        vec3 screen_coords = to_screen * vec3(vertex_coords + vec2(x_offset, y_offset), 1);\n\
        gl_Position = vec4(screen_coords.x, screen_coords.y, .5, 1);\n\
    }\n";

static const char *image_fragment_shader = "\n\
    varying vec2 raster_coords;\n\
    uniform vec2 image_coord_scale;\n\
    uniform sampler2D image;\n\
    \n\
    void main()\n\
    {\n\
        ivec2 tc = ivec2(raster_coords.x, raster_coords.y);\n\
        vec3 pixel = texture2D(image, raster_coords * image_coord_scale).xyz;\n\
        gl_FragColor = vec4(pixel, 1); \n\
    }\n";


void initialize_gl(void)
{
#if defined(__linux__)
    glewInit();
#endif // defined(__linux__)

    glClearColor(0, 0, 0, 1);
    CheckOpenGL(__FILE__, __LINE__);

    GLuint va;
    glGenVertexArrays(1, &va);
    glBindVertexArray(va);
    CheckOpenGL(__FILE__, __LINE__);

    image_program = GenerateProgram("image", hires_vertex_shader, image_fragment_shader);
    assert(image_program != 0);
    glBindAttribLocation(image_program, raster_coords_attrib, "vertex_coords");
    CheckOpenGL(__FILE__, __LINE__);

    image_texture_location = glGetUniformLocation(image_program, "image");
    image_texture_coord_scale_location = glGetUniformLocation(image_program, "image_coord_scale");
    image_to_screen_location = glGetUniformLocation(image_program, "to_screen");
    image_x_offset_location = glGetUniformLocation(image_program, "x_offset");
    image_y_offset_location = glGetUniformLocation(image_program, "y_offset");

    // initialize_screen_areas();
    CheckOpenGL(__FILE__, __LINE__);

    screen_image = initialize_texture(SCREEN_X, SCREEN_Y, NULL);
    screen_image_rectangle.push_back({make_rectangle_array_buffer(0, 0, SCREEN_X, SCREEN_X), raster_coords_attrib, 2, GL_FLOAT, GL_FALSE, 0});
}

void set_image_shader(float to_screen[9], const opengl_texture& texture, float x, float y)
{
    glUseProgram(image_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform2f(image_texture_coord_scale_location, 1.0 / texture.w, 1.0 / texture.h);
    glUniform1i(image_texture_location, 0);
    glUniformMatrix3fv(image_to_screen_location, 1, GL_FALSE, to_screen);
    glUniform1f(image_x_offset_location, x);
    glUniform1f(image_y_offset_location, y);
}

static void redraw(GLFWwindow *window)
{
    int fbw, fbh;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    glViewport(0, 0, fbw, fbh);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, screen_image);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCREEN_X, SCREEN_Y, 0, GL_RGBA, GL_UNSIGNED_BYTE, framebuffer);
    set_image_shader(to_screen_transform, screen_image, 0, 0);

    screen_image_rectangle.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    CheckOpenGL(__FILE__, __LINE__);
}

static void error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW: %s\n", description);
}

static void key(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    auto set_bits = [](uint8_t& data, uint8_t bits) { data = data | bits; };
    auto clear_bits = [](uint8_t& data, uint8_t bits) { data = data & ~bits; };
    auto set_bitfield = [](uint8_t& data, uint8_t mask, uint8_t bits) { data = (data & ~mask) | bits; };

    static bool shift_pressed = false;

    if(action == GLFW_PRESS || action == GLFW_REPEAT ) {
        switch(key) {
            case GLFW_KEY_RIGHT_SHIFT:
            case GLFW_KEY_LEFT_SHIFT:
                shift_pressed = true;
                break;
            case GLFW_KEY_V:
                save_vdp = true;
                break;
            case GLFW_KEY_N:
                do_save_images_on_vdp_write = !do_save_images_on_vdp_write;
                break;
            case GLFW_KEY_W:
                set_bits(controller_1_joystick_state, CONTROLLER1_NORTH_BIT);
                break;
            case GLFW_KEY_A:
                set_bits(controller_1_joystick_state, CONTROLLER1_WEST_BIT);
                break;
            case GLFW_KEY_S:
                set_bits(controller_1_joystick_state, CONTROLLER1_SOUTH_BIT);
                break;
            case GLFW_KEY_D:
                set_bits(controller_1_joystick_state, CONTROLLER1_EAST_BIT);
                break;
            case GLFW_KEY_SPACE:
                set_bits(controller_1_joystick_state, CONTROLLER1_FIRE_LEFT_BIT);
                break;
            case GLFW_KEY_ENTER:
                set_bits(controller_1_keypad_state, CONTROLLER1_FIRE_RIGHT_BIT);
                break;
            case GLFW_KEY_0:
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_0);
                break;
            case GLFW_KEY_1:
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_1);
                break;
            case GLFW_KEY_2:
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_2);
                break;
            case GLFW_KEY_3:
                if(shift_pressed) {
                    set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_pound);
                } else {
                    set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_3);
                }
                break;
            case GLFW_KEY_4:
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_4);
                break;
            case GLFW_KEY_5:
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_5);
                break;
            case GLFW_KEY_6:
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_6);
                break;
            case GLFW_KEY_7:
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_7);
                break;
            case GLFW_KEY_8:
                if(shift_pressed) {
                    set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_asterisk);
                } else {
                    set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_8);
                }
                break;
            case GLFW_KEY_9:
                set_bitfield(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK, CONTROLLER1_KEYPAD_9);
                break;
        }
    } else if(action == GLFW_RELEASE) {
        switch(key) {
            case GLFW_KEY_R:
                Z80Reset(&z80state);
                break;
            case GLFW_KEY_RIGHT_SHIFT:
            case GLFW_KEY_LEFT_SHIFT:
                shift_pressed = false;
                break;
            case GLFW_KEY_W:
                clear_bits(controller_1_joystick_state, CONTROLLER1_NORTH_BIT);
                break;
            case GLFW_KEY_A:
                clear_bits(controller_1_joystick_state, CONTROLLER1_WEST_BIT);
                break;
            case GLFW_KEY_S:
                clear_bits(controller_1_joystick_state, CONTROLLER1_SOUTH_BIT);
                break;
            case GLFW_KEY_D:
                clear_bits(controller_1_joystick_state, CONTROLLER1_EAST_BIT);
                break;
            case GLFW_KEY_SPACE:
                clear_bits(controller_1_joystick_state, CONTROLLER1_FIRE_LEFT_BIT);
                break;
            case GLFW_KEY_ENTER:
                clear_bits(controller_1_keypad_state, CONTROLLER1_FIRE_RIGHT_BIT);
                break;
            case GLFW_KEY_0:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_1:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_2:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_3:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_4:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_5:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_6:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_7:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_8:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
            case GLFW_KEY_9:
                clear_bits(controller_1_keypad_state, CONTROLLER1_KEYPAD_MASK);
                break;
        }
    }
}

static void resize_based_on_window(GLFWwindow *window)
{
    glfwGetWindowSize(window, &gWindowWidth, &gWindowHeight);
    float cw = 256, ch = 192;
    if(float(gWindowHeight) / gWindowWidth < ch / cw) {
        pixel_to_ui_scale = gWindowHeight / ch;
    } else {
        pixel_to_ui_scale = gWindowWidth / cw;
    }
    make_to_screen_transform();
}

static void resize(GLFWwindow *window, int x, int y)
{
    resize_based_on_window(window);
    int fbw, fbh;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    glViewport(0, 0, fbw, fbh);
}

static void button(GLFWwindow *window, int b, int action, int mods)
{
    double x, y;
    glfwGetCursorPos(window, &x, &y);

    if(b == GLFW_MOUSE_BUTTON_1 && action == GLFW_PRESS) {
        gButtonPressed = 1;
	gOldMouseX = x;
	gOldMouseY = y;

        // TODO: button press
    } else {
        gButtonPressed = -1;
        // TODO: button release
    }
    redraw(window);
}

static void motion(GLFWwindow *window, double x, double y)
{
    // to handle https://github.com/glfw/glfw/issues/161
    // If no motion has been reported yet, we catch the first motion
    // reported and store the current location
    if(!gMotionReported) {
        gMotionReported = true;
        gOldMouseX = x;
        gOldMouseY = y;
    }

    gOldMouseX = x;
    gOldMouseY = y;

    if(gButtonPressed == 1) {
        // TODO motion while dragging
    } else {
        // TODO motion while not dragging
    }
    redraw(window);
}

void load_joystick_setup()
{
    FILE *fp = fopen("joystick.ini", "r");
    if(fp == NULL) {
        fprintf(stderr,"no joystick.ini file found, assuming defaults\n");
        fprintf(stderr,"store GLFW joystick buttons for N, S, E, W, FireLeft, FireRight in joystick.ini\n");
        fprintf(stderr,"e.g. \"21 23 22 24 1 2\" for Samsung EI-GP20\n");
        return;
    }
    if(fscanf(fp, "%d %d %d %d %d %d", &joystick_button_north, &joystick_button_south, &joystick_button_east, &joystick_button_west, &joystick_button_fire_left, &joystick_button_fire_right) != 6) {
        fprintf(stderr,"couldn't parse joystick.ini\n");
        fprintf(stderr,"store GLFW joystick buttons for N, S, E, W, FireLeft, FireRight in joystick.ini\n");
        fprintf(stderr,"e.g. \"21 23 22 24 1 2\" for Samsung EI-GP20\n");
    }
    fclose(fp);
}

void iterate_ui()
{
    auto set_bits = [](uint8_t& data, uint8_t bits) { data = data | bits; };
    auto clear_bits = [](uint8_t& data, uint8_t bits) { data = data & ~bits; };

    CheckOpenGL(__FILE__, __LINE__);
    if(glfwWindowShouldClose(my_window)) {
        quit = true;
        return;
    }

    CheckOpenGL(__FILE__, __LINE__);
    redraw(my_window);
    CheckOpenGL(__FILE__, __LINE__);
    glfwSwapBuffers(my_window);
    CheckOpenGL(__FILE__, __LINE__);

    if(false) {
        for(int i = 0; i < 16; i++) {
            if(glfwJoystickPresent(GLFW_JOYSTICK_1 + i)) {
                printf("JOYSTICK_%d present\n", 1 + i);
            } else {
                printf("JOYSTICK_%d not present\n", 1 + i);
            }
        }
    }

    if(glfwJoystickPresent(GLFW_JOYSTICK_1)) {
        if(false) printf("joystick 1 present\n");

        int button_count;
        const unsigned char* buttons = glfwGetJoystickButtons(GLFW_JOYSTICK_1, &button_count);

        if(false)
            for(int i = 0; i < button_count; i++) {
                printf("Button %d: %s\n", i, (buttons[i] == GLFW_PRESS) ? "pressed" : "not pressed");
            }

        if(
            button_count <= joystick_button_north &&
            button_count <= joystick_button_south &&
            button_count <= joystick_button_east &&
            button_count <= joystick_button_west &&
            button_count <= joystick_button_fire_left &&
            button_count <= joystick_button_fire_right
            ){

            fprintf(stderr, "couldn't map gamepad buttons\n");
            use_joystick = false;

        } else  {

            clear_bits(controller_1_joystick_state, CONTROLLER1_EAST_BIT | CONTROLLER1_WEST_BIT | CONTROLLER1_NORTH_BIT | CONTROLLER1_SOUTH_BIT | CONTROLLER1_FIRE_LEFT_BIT );
            clear_bits(controller_1_keypad_state, CONTROLLER1_FIRE_RIGHT_BIT);

            if(buttons[joystick_button_west] == GLFW_PRESS) {
                set_bits(controller_1_joystick_state, CONTROLLER1_WEST_BIT);
            }
            if(buttons[joystick_button_east] == GLFW_PRESS) {
                set_bits(controller_1_joystick_state, CONTROLLER1_EAST_BIT);
            }
            if(buttons[joystick_button_north] == GLFW_PRESS) {
                set_bits(controller_1_joystick_state, CONTROLLER1_NORTH_BIT);
            }
            if(buttons[joystick_button_south] == GLFW_PRESS) {
                set_bits(controller_1_joystick_state, CONTROLLER1_SOUTH_BIT);
            }
            if(buttons[joystick_button_fire_left] == GLFW_PRESS) {
                set_bits(controller_1_joystick_state, CONTROLLER1_FIRE_LEFT_BIT);
            }
            if(buttons[joystick_button_fire_right] == GLFW_PRESS) {
                set_bits(controller_1_keypad_state, CONTROLLER1_FIRE_RIGHT_BIT);
            }

            use_joystick = true;
        }

    } else {
        use_joystick = false;
    }

    glfwPollEvents();
}

void shutdown_ui()
{
    glfwTerminate();
}

void initialize_ui()
{
    load_joystick_setup();

    glfwSetErrorCallback(error_callback);

    if(!glfwInit()) {
        exit(EXIT_FAILURE);
    }

    // glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    // glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    // glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    // glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); 

    // glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_DOUBLEBUFFER, 1);
    my_window = glfwCreateWindow(SCREEN_X * SCREEN_SCALE, SCREEN_Y * SCREEN_SCALE, "ColecoVision", NULL, NULL);
    if (!my_window) {
        glfwTerminate();
        fprintf(stdout, "Couldn't open main window\n");
        exit(EXIT_FAILURE);
    }

    glfwMakeContextCurrent(my_window);
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION: %s\n", glGetString(GL_VERSION));

    glfwGetWindowSize(my_window, &gWindowWidth, &gWindowHeight);
    make_to_screen_transform();
    initialize_gl();
    resize_based_on_window(my_window);
    CheckOpenGL(__FILE__, __LINE__);

    glfwSetKeyCallback(my_window, key);
    glfwSetMouseButtonCallback(my_window, button);
    glfwSetCursorPosCallback(my_window, motion);
    glfwSetFramebufferSizeCallback(my_window, resize);
    glfwSetWindowRefreshCallback(my_window, redraw);
    CheckOpenGL(__FILE__, __LINE__);
}

#ifdef __linux__

static const char *i2c_devname = "/dev/i2c-1";
static int cvhat_fd = -1;

#define CVHAT_ADDRESS 0x5A

int cvhat_init()
{
    cvhat_fd = open(i2c_devname, O_RDWR);
    if (cvhat_fd < 0) {
        perror("open");
        return 0;
    }

    int addr = CVHAT_ADDRESS; /* The I2C address */

    if (ioctl(cvhat_fd, I2C_SLAVE, addr) < 0) {
	close(cvhat_fd);
	cvhat_fd = -1;
        perror("ioctl");
        return 0;
    }

    return 1;
}

const uint8_t CVHAT_JOYSTICK_1 = 0x20;
const uint8_t CVHAT_JOYSTICK_1_CHANGED = 0x21;
const uint8_t CVHAT_KEYPAD_1 = 0x22;
const uint8_t CVHAT_KEYPAD_1_CHANGED = 0x23;
const uint8_t CVHAT_JOYSTICK_2 = 0x24;
const uint8_t CVHAT_JOYSTICK_2_CHANGED = 0x25;
const uint8_t CVHAT_KEYPAD_2 = 0x26;
const uint8_t CVHAT_KEYPAD_2_CHANGED = 0x27;

uint8_t read_u8(int fd, int reg)
{
    unsigned char buf[1];
    buf[0] = reg;
    if (write(fd, buf, 1) != 1) {
        fprintf(stderr, "write register number failed\n");
        exit(EXIT_FAILURE);
    }

    if (read(fd, buf, 1) != 1) {
        fprintf(stderr, "read register value failed\n");
        exit(EXIT_FAILURE);
    }
    return buf[0];
}

void cvhat_read_controllers()
{
    if(!(cvhat_fd < 0)) {
	uint8_t joy1 = read_u8(cvhat_fd, CVHAT_JOYSTICK_1);
	uint8_t key1 = read_u8(cvhat_fd, CVHAT_KEYPAD_1);
	uint8_t joy2 = read_u8(cvhat_fd, CVHAT_JOYSTICK_2);
	uint8_t key2 = read_u8(cvhat_fd, CVHAT_KEYPAD_2);
#error need to convert to CV joystick and keypad state
	user_flags = (joy1 << 0) | (key1 << 8) |
	    (joy2 << 16) | (key2 << 24);
    }
}

#endif

void do_vdp_test(const char *vdp_dump_name, const char *image_name)
{
    TMS9918A::register_file_t registers;
    std::array<uint8_t, 16384> memory;

    FILE *vdp_dump_in = fopen(vdp_dump_name, "r");
    char line[512];
    fgets(line, sizeof(line), vdp_dump_in);
    for(size_t i = 0; i < 8; i++) {
        int v;
        fscanf(vdp_dump_in, " %u", &v);
        registers[i] = v;
    }
    for(size_t i = 0; i < 16384; i++) {
        int v;
        fscanf(vdp_dump_in, " %u", &v);
        memory[i] = v;
    }
    fclose(vdp_dump_in);

    static uint8_t framebuffer[SCREEN_X * SCREEN_Y * 4];
    auto pixel_setter = [](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t *pixel = framebuffer + 4 * (x + y * SCREEN_X) + 0;
        set_color(pixel, r, g, b);
    };
    CreateImageAndReturnFlags(registers, memory, pixel_setter);
    FILE *fp = fopen(image_name, "wb");
    write_image(framebuffer, fp);
    fclose(fp);
}

int main(int argc, char **argv)
{
#ifdef PROVIDE_DEBUGGER
    bool do_debugger = false;
    char *debugger_argument = NULL;

    populate_command_handlers();
#endif

    char *progname = argv[0];
    argc -= 1;
    argv += 1;

    while((argc > 0) && (argv[0][0] == '-')) {
	if(
            (strcmp(argv[0], "-help") == 0) ||
            (strcmp(argv[0], "-h") == 0) ||
            (strcmp(argv[0], "-?") == 0))
         {
             usage(progname);
             exit(EXIT_SUCCESS);
	} else if(strcmp(argv[0], "-vdp-test") == 0) {
            if(argc < 3) {
                fprintf(stderr, "-vdp-test requires VDP register dump filename and output image filename\n");
                usage(progname);
                exit(EXIT_FAILURE);
            }
            do_vdp_test(argv[1], argv[2]);
            exit(0);
#ifdef PROVIDE_DEBUGGER
	} else if(strcmp(argv[0], "-debugger") == 0) {
            if(argc < 2) {
                fprintf(stderr, "-debugger requires initial commands (can be empty, e.g. \"\"\n");
                usage(progname);
                exit(EXIT_FAILURE);
            }
            do_debugger = true;
            debugger_argument = argv[1];
	    argc -= 2;
	    argv += 2;
#endif
	} else {
	    fprintf(stderr, "unknown parameter \"%s\"\n", argv[0]);
            usage(progname);
	    exit(EXIT_FAILURE);
	}
    }

    if(argc < 2) {
        usage(progname);
        exit(EXIT_FAILURE);
    }

#ifdef __linux__
    if(!cvhat_init()) {
	printf("couldn't connect to colecovision controller HAT.\n");
    }
#endif

    initialize_ui();
    COLECOinterface::Start();

    static unsigned char rom_temp[65536];
    FILE *fp;

    char *bios_name = argv[0];
    char *cart_name = argv[1];

    fp = fopen(bios_name, "rb");
    if(fp == NULL) {
        fprintf(stderr, "failed to open %s for reading\n", bios_name);
        exit(EXIT_FAILURE);
    }
    size_t bios_length = fread(rom_temp, 1, sizeof(rom_temp), fp);
    if(bios_length != 0x2000) {
        fprintf(stderr, "ROM read from %s was unexpectedly %zd bytes\n", bios_name, bios_length);
        exit(EXIT_FAILURE);
    }
    fclose(fp);
    ROMboard *bios_rom = new ROMboard(0, bios_length, rom_temp);

    audio_flush_func audio_flush = [](uint8_t *buf, size_t sz){ COLECOinterface::EnqueueAudioSamples(buf, sz); };

    fp = fopen(cart_name, "rb");
    if(fp == NULL) {
        fprintf(stderr, "failed to open %s for reading\n", cart_name);
        exit(EXIT_FAILURE);
    }
    size_t cart_length = fread(rom_temp, 1, sizeof(rom_temp), fp);
    if(cart_length < 0x2000) {
        fprintf(stderr, "ROM read from %s was unexpectedly short (%zd bytes)\n", cart_name, cart_length);
        exit(EXIT_FAILURE);
    }
    fclose(fp);
    ROMboard *cart_rom = new ROMboard(0x8000, cart_length, rom_temp);

    clk_t clk = 0;
    ColecoHW* colecohw = new ColecoHW(COLECOinterface::GetAudioSampleRate(), COLECOinterface::GetPreferredAudioBufferSampleCount());

#ifdef PROVIDE_DEBUGGER
    Debugger *debugger = NULL;
    if(do_debugger) {
        debugger = new Debugger(colecohw, clk);
    }
#endif

    boards.push_back(colecohw);
    boards.push_back(bios_rom);
    boards.push_back(cart_rom);
    boards.push_back(new RAMboard(0x6000, 0x2000));

    for(auto b = boards.begin(); b != boards.end(); b++) {
        (*b)->init();
    }

    memset(&z80state, 0, sizeof(z80state));

    Z80Reset(&z80state);

#ifdef PROVIDE_DEBUGGER
    if(debugger) {
        enter_debugger = true;
        debugger->process_line(boards, &z80state, debugger_argument);
    }
#endif

    std::chrono::time_point<std::chrono::system_clock> then = std::chrono::system_clock::now();
    bool nmi_was_issued = false;

    while(!quit)
    {
#ifdef PROVIDE_DEBUGGER
        if(debugger && (enter_debugger || debugger->should_debug(boards, &z80state))) {
            debugger->go(stdin, boards, &z80state);
            enter_debugger = false;
        } else
#endif
        {
            std::chrono::time_point<std::chrono::system_clock> before = std::chrono::system_clock::now();

            clk_t start_of_this_slice = clk;
            static clk_t previous_field_start_clock = 0;

            // XXX THIS HAS TO REMAIN 1 UNTIL I CAN ISSUE NonMaskableInterrupt PER-INSTRUCTION
            constexpr int iterated_clock_quantum = 1;

            if(false) {
                clk += Z80Emulate(&z80state, clocks_per_slice - 1000);
            }

            while((clk - start_of_this_slice) < clocks_per_slice) {
                clk_t clocks_this_step = Z80Emulate(&z80state, iterated_clock_quantum);
#ifdef PROVIDE_DEBUGGER
                if(debugger) {
                    if(enter_debugger || debugger->should_debug(boards, &z80state)) {
                        debugger->go(stdin, boards, &z80state);
                        enter_debugger = false;
                    }
                }
#endif
                clk += clocks_this_step;

                uint64_t retrace_before = previous_field_start_clock / clocks_per_slice;
                uint64_t retrace_after = clk / clocks_per_slice;
                if(retrace_before != retrace_after) {
                    // printf("VDP frame interrupt %llu, %llu clocks\n", retrace_after, clk - previous_field_start_clock);
                    {
                        std::chrono::time_point<std::chrono::system_clock> before = std::chrono::system_clock::now();
                        colecohw->vdp.perform_scanout(framebuffer);
                        if(save_vdp) {
                            static int which = 0;
                            static char filename[512];
                            sprintf(filename, "%s_%02d.vdp", getenv("VDP_OUT_BASE"), which);
                            FILE *vdp_file = fopen(filename, "w");
                            fprintf(vdp_file, "# %s_%02d.vdp, 8 register bytes, 16384 RAM bytes\n", getenv("VDP_OUT_BASE"), which++);
                            for(size_t i = 0; i < colecohw->vdp.registers.size(); i++) {
                                bool more_in_row = ((i + 1) % 8) != 0;
                                fprintf(vdp_file, "%u%s", colecohw->vdp.registers[i], more_in_row ? " " : "\n");
                            }
                            fputs("", vdp_file);
                            for(size_t i = 0; i < colecohw->vdp.memory.size(); i++) {
                                bool more_in_row = ((i + 1) % 16) != 0;
                                fprintf(vdp_file, "%u%s", colecohw->vdp.memory[i], more_in_row ? " " : "\n");
                            }
                            fputs("", vdp_file);
                            save_vdp = false;
                        }
                        std::chrono::time_point<std::chrono::system_clock> after = std::chrono::system_clock::now();
                        auto real_elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(after - before);
                        if(profiling) printf("VDP scanout %lld\n", real_elapsed_micros.count());
                    }

                    colecohw->vdp.vsync();
                    previous_field_start_clock = clk;
                }

                if(colecohw->nmi_required()) {
                    if(!nmi_was_issued) {
                        Z80NonMaskableInterrupt (&z80state);
                        nmi_was_issued = true;
                    }
                } else {
                    nmi_was_issued = false;
                }
            }
            std::chrono::time_point<std::chrono::system_clock> after = std::chrono::system_clock::now();
            auto real_elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(after - before);
            if(profiling) printf("insns %lld\n", real_elapsed_micros.count());

            std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();

            auto elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(now - then);
            if(!run_fast || pause_cpu) {
                // std::this_thread::sleep_for(std::chrono::microseconds(clocks_per_slice * 1000000 / machine_clock_rate) - elapsed_micros);
                auto remaining_in_slice = std::chrono::microseconds(micros_per_slice) - elapsed_micros;
                if(profiling) printf("elapsed %lld, sleep %lld\n", elapsed_micros.count(), remaining_in_slice.count());
                // printf("%f%%\n", 100.0 - elapsed_micros.count() * 100.0 / micros_per_slice);
            }

            then = now;
        }

	std::chrono::time_point<std::chrono::system_clock> before = std::chrono::system_clock::now();
        for(auto b = boards.begin(); b != boards.end(); b++) {
            int irq;
            if((*b)->board_get_interrupt(irq)) {
                // Pretend to be 8259 configured for Alice2:
                Z80_INTERRUPT_FETCH = true;
                Z80_INTERRUPT_FETCH_DATA = 0x3f00 + irq * 4;
                clk += Z80Interrupt(&z80state, 0xCD);
                break;
            }
        }

	std::chrono::time_point<std::chrono::system_clock> after = std::chrono::system_clock::now();
	auto real_elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(after - before);
	if(profiling) printf("interrupts %lld\n", real_elapsed_micros.count());

	before = std::chrono::system_clock::now();
        for(auto b = boards.begin(); b != boards.end(); b++) {
            (*b)->idle();
        }
	after = std::chrono::system_clock::now();
	real_elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(after - before);
	if(profiling) printf("idle %lld\n", real_elapsed_micros.count());

	before = std::chrono::system_clock::now();
        colecohw->fill_flush_audio(clk, audio_flush);
	after = std::chrono::system_clock::now();
	real_elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(after - before);
	if(profiling) printf("audio %lld\n", real_elapsed_micros.count());

	before = std::chrono::system_clock::now();
        iterate_ui();
	after = std::chrono::system_clock::now();
	real_elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(after - before);
	if(profiling) printf("UI %lld\n", real_elapsed_micros.count());

#ifdef __linux__
        if(cvhat_fd >= 0) {
            before = std::chrono::system_clock::now();
            cvhat_read_controllers();
            after = std::chrono::system_clock::now();
            real_elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(after - before);
            if(profiling) printf("CVHAT I2C %lld\n", real_elapsed_micros.count());
        }
#endif

    }

    shutdown_ui();

    return 0;
}
