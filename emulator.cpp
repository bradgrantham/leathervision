#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <string>
#include <map>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>

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

const bool debug = false;

unsigned long user_flags = 0;

bool Z80_INTERRUPT_FETCH = false;
unsigned short Z80_INTERRUPT_FETCH_DATA;

const long long machine_clock_rate = 3579545;
unsigned long long clk = 0;
long long micros_per_slice = 16666;
volatile bool run_fast = false;
volatile bool pause_cpu = false;

ao_device *aodev;

std::vector<board_base*> boards;

bool quit = false;

const int SCREEN_X = 256;
const int SCREEN_Y = 192;
const int SCREEN_SCALE = 3;

unsigned char framebuffer[SCREEN_X * SCREEN_SCALE * SCREEN_Y * SCREEN_SCALE * 4];

void write_image(unsigned char image[SCREEN_X * SCREEN_SCALE * SCREEN_Y * SCREEN_SCALE * 4], FILE *fp)
{
    fprintf(fp, "P6 %d %d 255\n", SCREEN_X * SCREEN_SCALE, SCREEN_Y * SCREEN_SCALE);
    for(int row = 0; row < SCREEN_Y * SCREEN_SCALE; row++) {
        for(int col = 0; col < SCREEN_X * SCREEN_SCALE; col++) {
            fwrite(image + (row * SCREEN_X * SCREEN_SCALE + col) * 4, 3, 1, fp);
        }
    }
}

unsigned char nybbles_to_color[16][3] = {
    {0, 0, 0},
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

void nybble_to_color(unsigned int nybble, unsigned char color[3])
{
    if(nybble == 0)
        return;
    color[0] = nybbles_to_color[nybble][0];
    color[1] = nybbles_to_color[nybble][1];
    color[2] = nybbles_to_color[nybble][2];
}

typedef std::function<void (char *audiobuffer, size_t dist)> audio_flush_func;
audio_flush_func audio_flush;

unsigned int att_table[] = {
    256, 203, 161, 128, 101, 80, 64, 51, 40, 32, 25, 20, 16, 12, 10, 0,
};

unsigned char scale_by_attenuation_flags(unsigned int att, unsigned char value)
{
    return int(value) * att_table[att] / 256;
}

struct SN76489A
{
    bool debug = false;
    unsigned int clock_rate;

    int phase = 0;

    unsigned char cmd_latched = 0;
    static const int CMD_BIT = 0x80;
    static const int CMD_REG_MASK = 0x70;
    static const int DATA_MASK = 0x0F;
    static const int CMD_REG_SHIFT = 4;
    static const int FREQ_HIGH_SHIFT = 4;
    static const int FREQ_HIGH_MASK = 0x3F;
    static const int CMD_NOISE_CONFIG_MASK = 0x04;
    static const int CMD_NOISE_CONFIG_SHIFT = 2;
    static const int CMD_NOISE_FREQ_MASK = 0x03;

    unsigned int tone_lengths[3];
    unsigned int tone_attenuation[3];

    unsigned int noise_config;
    unsigned int noise_length;
    unsigned int noise_length_id;
    unsigned int noise_attenuation;

    unsigned int tone_counters[3];
    unsigned int tone_bit[3];
    unsigned int noise_counter;

    uint16_t noise_register = 0x8000;
    unsigned int noise_flipflop = 0;

    unsigned long long previous_clock;

    SN76489A(unsigned int clock_rate_) :
        clock_rate(clock_rate_)
    {
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
                if(noise_length_id == 0)
                    noise_length = 512;
                else if(noise_length_id == 1)
                    noise_length = 1024;
                else if(noise_length_id == 2)
                    noise_length = 2048;
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

    unsigned long long calc_flip_count(unsigned long long previous_clock, unsigned long long current_clock, unsigned int previous_counter, unsigned int length)
    {
        if(length < 1)
            return 0;
        unsigned long long clocks = current_clock - previous_clock;
        unsigned long long flips = (previous_counter + clocks) / length;
        return flips;
    }

    void advance_noise_to_clock(unsigned long long flips)
    {
        for(int i = 0; i < flips; i++) {
            noise_flipflop ^= 1;

            if(noise_flipflop) {
                int noise_bit = noise_register & 0x1;
                int new_bit;

                if(noise_config == 1)
                    new_bit = (noise_register & 0x1) ^ ((noise_register & 0x8) >> 3);
                else
                    new_bit = noise_bit;

                noise_register = (noise_register >> 1) | (new_bit << 15);
            }
        }
    }

    void advance_to_clock(unsigned long long clk)
    {
        unsigned long long tone_flips[3];
        
        for(int i = 0; i < 3; i++) {
            tone_flips[i] = calc_flip_count(previous_clock, clk, tone_counters[i], tone_lengths[i]);
        }

        int flips;
        if(noise_length_id == 3)
            flips = tone_flips[2];
        else
            flips = calc_flip_count(previous_clock, clk, noise_counter, noise_length);
        advance_noise_to_clock(flips);

        for(int i = 0; i < 3; i++) {
            tone_bit[i] = tone_bit[i] ^ (tone_flips[i] & 0x1);
            if(tone_lengths[i] > 0)
                tone_counters[i] = (tone_counters[i] + (clk - previous_clock)) % tone_lengths[i];
        }
        if(noise_length > 0)
            noise_counter = (noise_counter + (clk - previous_clock)) % noise_length;

        previous_clock = clk;
    }

    unsigned char get_level()
    {
        unsigned char v =
            scale_by_attenuation_flags(tone_attenuation[0], tone_bit[0] ? 0 : 64) + 
            + scale_by_attenuation_flags(tone_attenuation[1], tone_bit[1] ? 0 : 64)
            + scale_by_attenuation_flags(tone_attenuation[2], tone_bit[2] ? 0 : 64)
            + scale_by_attenuation_flags(noise_attenuation, (noise_register & 0x1) ? 0 : 64);
            ;

        return v;
    }

    static const int sample_rate = 44100;
    static const size_t audio_buffer_size = sample_rate / 100;
    char audio_buffer[audio_buffer_size];
    long long audio_buffer_next_sample = 0;

    void generate_audio(unsigned long long clk, audio_flush_func audio_flush)
    {
        for(long long c = previous_clock; c < clk; c++) {

            long long current_audio_sample = c * sample_rate / clock_rate;
            long long next_audio_sample = (c + 1) * sample_rate / clock_rate;

            if(next_audio_sample > current_audio_sample) {
                advance_to_clock(c);

                audio_buffer[audio_buffer_next_sample++] = get_level();

                if(audio_buffer_next_sample == audio_buffer_size) {
                    audio_flush(audio_buffer, audio_buffer_size);
                    audio_buffer_next_sample = 0;
                }
            }
        }

        previous_clock = clk;
    }
};

struct TMS9918A
{
    bool debug = false;

    static const int MEMORY_SIZE = 16384;
    unsigned char memory[16384];
    unsigned char registers[64];

    static const int REG_A0_A5_MASK = 0x3F;
    static const int CMD_MASK = 0xC0;
    static const int CMD_SET_REGISTER = 0x80;
    static const int CMD_SET_WRITE_ADDRESS = 0x40;
    static const int CMD_SET_READ_ADDRESS = 0x00;

    static const int VR0_BITMAP_MASK = 0x02;
    static const int VR0_EXTVID_MASK = 0x01;

    static const int VR1_16K_MASK = 0x80;
    static const int VR1_BLANK_MASK = 0x40;
    static const int VR1_INT_MASK = 0x20;
    static const int VR1_MULTIC_MASK = 0x10;
    static const int VR1_TEXT_MASK = 0x08;
    static const int VR1_SIZE4_MASK = 0x02;
    static const int VR1_MAG2X_MASK = 0x01;

    static const int VR2_SCREENIMAGE_MASK = 0x0F;
    static const int VR2_SCREENIMAGE_SHIFT = 10;

    static const int VR3_COLORTABLE_MASK_STANDARD = 0xFF;
    static const int VR3_COLORTABLE_SHIFT_STANDARD = 6;

    static const int VR3_COLORTABLE_MASK_BITMAP = 0x80;
    static const int VR3_COLORTABLE_SHIFT_BITMAP = 6;

    static const int VR3_ADDRESS_MASK_BITMAP = 0x7F;
    static const int VR3_ADDRESS_MASK_SHIFT = 6;

    static const int VR4_PATTERN_MASK_STANDARD = 0x07;
    static const int VR4_PATTERN_SHIFT_STANDARD = 11;

    static const int VR4_PATTERN_MASK_BITMAP = 0x04;
    static const int VR4_PATTERN_SHIFT_BITMAP = 11;

    static const int VR5_SPRITE_ATTR_MASK = 0x7F;
    static const int VR5_SPRITE_ATTR_SHIFT = 7;

    static const int VR6_SPRITE_PATTERN_MASK = 0x07;
    static const int VR6_SPRITE_PATTERN_SHIFT = 11;

    static const int VDP_STATUS_INT_BIT = 0x80;
    static const int VDP_STATUS_COINC_BIT = 0x20;

    static const int ROW_SHIFT = 5;
    static const int THIRD_SHIFT = 11;
    static const int CHARACTER_PATTERN_SHIFT = 3;
    static const int CHARACTER_COLOR_SHIFT = 3;
    static const int ADDRESS_MASK_FILL = 0x3F;


    static const int SPRITE_EARLY_CLOCK_MASK = 0x80;
    static const int SPRITE_COLOR_MASK = 0x0F;
    static const int SPRITE_NAME_SHIFT = 3;
    static const int SPRITE_NAME_MASK_SIZE4 = 0xFC;

    enum {CMD_PHASE_FIRST, CMD_PHASE_SECOND} cmd_phase = CMD_PHASE_FIRST;
    unsigned char cmd_data = 0x0;
    unsigned int read_address = 0x0;
    unsigned int write_address = 0x0;

    bool vdp_int = false;
    bool sprite_int = false;

    TMS9918A()
    {
        memset(memory, 0, MEMORY_SIZE);
        memset(registers, 0, 64);
    }

    void write(int cmd, unsigned char data)
    {
        if(cmd) {

            if(cmd_phase == CMD_PHASE_FIRST) {

                if(debug) printf("VDP command write, first byte 0x%02X\n", data);
                cmd_data = data;
                cmd_phase = CMD_PHASE_SECOND;

            } else {

                int cmd = data & CMD_MASK;
                if(cmd == CMD_SET_REGISTER) {
                    int which_register = data & REG_A0_A5_MASK;
                    if(debug) printf("VDP command write to register 0x%02X, value 0x%02X\n", which_register, cmd_data);
                    registers[which_register] = cmd_data;
                } else if(cmd == CMD_SET_WRITE_ADDRESS) {
                    // write_address = (cmd_data << 6) | (data & REG_A0_A5_MASK);
                    write_address = ((data & REG_A0_A5_MASK) << 8) | cmd_data;
                    if(debug) printf("VDP write address set to 0x%04X\n", write_address);
                } else if(cmd == CMD_SET_READ_ADDRESS) {
                    // read_address = (cmd_data << 6) | (data & REG_A0_A5_MASK);
                    read_address = ((data & REG_A0_A5_MASK) << 8) | cmd_data;
                    if(debug) printf("VDP read address set to 0x%04X\n", write_address);
                } else {
                    if(debug) printf("uh-oh, VDP cmd was 0x%02X!\n", cmd);
                }
                cmd_phase = CMD_PHASE_FIRST;
            }

        } else {

            if(debug) {
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

    unsigned char read(int cmd)
    {
        if(cmd) {
            cmd_phase = CMD_PHASE_FIRST;
            unsigned char data =
                (vdp_int ? VDP_STATUS_INT_BIT : 0x0) |
                (sprite_int ? VDP_STATUS_COINC_BIT : 0x0)
                ;
            vdp_int = false;
            sprite_int = false;
            return data;
        } else {
            unsigned char data = memory[read_address++];
            read_address = read_address % MEMORY_SIZE;
            return data;
        }
    }

    void get_color(int x, int y, unsigned char color[3])
    {
        bool bitmap_mode = (registers[0] & VR0_BITMAP_MASK);
        bool text_mode = (registers[1] & VR1_TEXT_MASK);
        bool multicolor_mode = (registers[1] & VR1_MULTIC_MASK);

        color[0] = 0;
        color[1] = 0;
        color[2] = 0;

        int col = x / 8;
        int row = y / 8;
        int pattern_col = x % 8;
        int pattern_row = y % 8;

        int which_color = 8;
        int pattern_address;
        int color_address;

        int screen_address = ((registers[2] & VR2_SCREENIMAGE_MASK) << VR2_SCREENIMAGE_SHIFT) | (row << ROW_SHIFT) | col;
        unsigned char pattern_name = memory[screen_address];

        bool sprites_valid = false;

        if(!bitmap_mode && !text_mode && !multicolor_mode) {
            // Standard mode

            pattern_address = ((registers[4] & VR4_PATTERN_MASK_STANDARD) << VR4_PATTERN_SHIFT_STANDARD) | (pattern_name << CHARACTER_PATTERN_SHIFT) | pattern_row;

            color_address = ((registers[3] & VR3_COLORTABLE_MASK_STANDARD) << VR3_COLORTABLE_SHIFT_STANDARD) | (pattern_name >> CHARACTER_COLOR_SHIFT);

            sprites_valid = true;

        } else if(bitmap_mode && !text_mode && !multicolor_mode) {
            // bitmap mode

            int third = (row / 8) << THIRD_SHIFT;

            int address_mask = ((registers[3] & VR3_ADDRESS_MASK_BITMAP) << VR3_ADDRESS_MASK_SHIFT) | ADDRESS_MASK_FILL;

            // pattern_address = ((((registers[4] & VR4_PATTERN_MASK_BITMAP) << VR4_PATTERN_SHIFT_BITMAP) | third | (pattern_name << CHARACTER_PATTERN_SHIFT)) & address_mask) | pattern_row;
            pattern_address = (((registers[4] & VR4_PATTERN_MASK_BITMAP) << VR4_PATTERN_SHIFT_BITMAP) | third | (pattern_name << CHARACTER_PATTERN_SHIFT)) | pattern_row;

            // color_address = (((registers[3] & VR3_COLORTABLE_MASK_BITMAP) << VR3_COLORTABLE_SHIFT_BITMAP) | third | (pattern_name << CHARACTER_PATTERN_SHIFT) | pattern_row) & address_mask;
            color_address = (((registers[3] & VR3_COLORTABLE_MASK_BITMAP) << VR3_COLORTABLE_SHIFT_BITMAP) | third | (pattern_name << CHARACTER_PATTERN_SHIFT) | pattern_row);

            sprites_valid = true;

        } else {

            printf("unhandled video mode %d %d %d\n",
                bitmap_mode ? 1 : 0,
                text_mode ? 1 : 0,
                multicolor_mode ? 1 : 0);

            color[0] = 255;
            color[1] = 0;
            color[2] = 0;
            return;
            // abort();
        }

        int bit = memory[pattern_address] & (0x80 >> pattern_col);

        unsigned int colortable = memory[color_address];
        
        which_color = bit ? ((colortable >> 4) & 0xf) : (colortable & 0xf);

        nybble_to_color(which_color, color);

        if(sprites_valid) {
            int sprite_table_address = (registers[5] & VR5_SPRITE_ATTR_MASK) << VR5_SPRITE_ATTR_SHIFT;
            bool mag2x = registers[1] & VR1_MAG2X_MASK;
            bool size4 = registers[1] & VR1_SIZE4_MASK;
            bool had_pixel = false;

            for(int i = 0; i < 32; i++) {
                unsigned char *sprite = memory + sprite_table_address + i * 4;
                int sprite_y = sprite[0] + 1;

                if(sprite[0] == 0xD0)
                    break; // So says http://www.unige.ch/medecine/nouspikel/ti99/tms9918a.htm

                int sprite_x = sprite[1];
                int sprite_name = sprite[2];
                bool sprite_earlyclock = sprite[3] & SPRITE_EARLY_CLOCK_MASK;
                int sprite_color = sprite[3] & SPRITE_COLOR_MASK;

                // printf("sprite %d: %d %d %d %d\n", i, sprite_x, sprite_y, sprite_name, sprite_color);

                if(sprite_earlyclock)
                    sprite_x -= 32;

                if(x >= sprite_x && y >= sprite_y) {
                    int within_sprite_x, within_sprite_y;

                    if(mag2x) {
                        within_sprite_x = (x - sprite_x) / 2;
                        within_sprite_y = (y - sprite_y) / 2;
                    } else {
                        within_sprite_x = x - sprite_x;
                        within_sprite_y = y - sprite_y;
                    }

                    if(size4) {
                        if((within_sprite_x < 16) && (within_sprite_y < 16)) {

                            int quadrant = within_sprite_y / 8 + (within_sprite_x / 8) * 2;
                            int within_quadrant_y = within_sprite_y % 8;
                            int within_quadrant_x = within_sprite_x % 8;
                            int masked_sprite = sprite_name & SPRITE_NAME_MASK_SIZE4;
                            int sprite_pattern_address = ((registers[6] & VR6_SPRITE_PATTERN_MASK) << VR6_SPRITE_PATTERN_SHIFT) | (masked_sprite << SPRITE_NAME_SHIFT) | (quadrant << 3) | within_quadrant_y;
                            bit = memory[sprite_pattern_address] & (0x80 >> within_quadrant_x);
                            if(bit) {
                                sprite_int = had_pixel;
                                nybble_to_color(sprite_color, color);
                                had_pixel = true;
                            }
                        }

                    } else if((within_sprite_x < 8) && (within_sprite_y < 8)) {

                        int sprite_pattern_address = ((registers[6] & VR6_SPRITE_PATTERN_MASK) << VR6_SPRITE_PATTERN_SHIFT) | (sprite_name << SPRITE_NAME_SHIFT) | within_sprite_y;
                        bit = memory[sprite_pattern_address] & (0x80 >> within_sprite_x);
                        if(bit) {
                            sprite_int = had_pixel;
                            nybble_to_color(sprite_color, color);
                            had_pixel = true;
                        }
                    }
                }
            }
        }
    }

    void perform_scanout(unsigned char image[SCREEN_X * SCREEN_SCALE * SCREEN_Y * SCREEN_SCALE * 4])
    {
        for(int row = 0; row < SCREEN_Y; row++) {
            for(int col = 0; col < SCREEN_X; col++) {
                unsigned char color[3];
                get_color(col, row, color);
                for(int j = 0; j < SCREEN_SCALE; j++) {
                    for(int i = 0; i < SCREEN_SCALE; i++) {
                        unsigned char *pixel = image + ((row * SCREEN_SCALE + j) * SCREEN_X * SCREEN_SCALE + col * SCREEN_SCALE + i) * 4;
                        for(int c = 0; c < 3; c++)
                            pixel[c] = color[c];
                    }
                }
            }
        }
        vdp_int = true;
    }

    bool nmi_requested()
    {
        return (registers[1] & VR1_INT_MASK) && vdp_int;
    }
};

TMS9918A *VDP;

struct ColecoHW : board_base
{
    bool debug;

    TMS9918A vdp;
    SN76489A sound;

    bool reading_joystick = true;

    // static const int PIC_PORT = 0;

    static const int VDP_DATA_PORT = 0xBE;
    static const int VDP_CMD_PORT = 0xBF;

    static const int SN76489A_PORT = 0xFF;

    static const int SWITCH_TO_KEYPAD_PORT = 0x80;
    static const int SWITCH_TO_JOYSTICK_PORT = 0xC0;
    static const int CONTROLLER1_PORT = 0xFC;
    static const int CONTROLLER2_PORT = 0xFF;

    ColecoHW() :
        sound(machine_clock_rate)
    {
        debug = false;
        VDP = &vdp;
    }

    virtual bool io_write(int addr, unsigned char data)
    {
        // if(addr == ColecoHW::PROPELLER_PORT) {
            // write_to_propeller(data);
            // return true;
        // }

        if(addr == ColecoHW::VDP_CMD_PORT) {
            vdp.write(1, data);
            return true;
        }

        if(addr == ColecoHW::VDP_DATA_PORT) {
            vdp.write(0, data);
            return true;
        }

        if(addr == ColecoHW::SN76489A_PORT) {
            if(debug) printf("audio write 0x%02X\n", data);
            sound.write(data);
            return true;
        }

        if(addr == ColecoHW::SWITCH_TO_KEYPAD_PORT) {
            if(debug) printf("switch to keypad\n");
            reading_joystick = false;
            return true;
        }

        if(addr == ColecoHW::SWITCH_TO_JOYSTICK_PORT) {
            if(debug) printf("switch to keypad\n");
            reading_joystick = true;
            return true;
        }

        return false;
    }

    virtual bool io_read(int addr, unsigned char &data)
    {
        if(addr == ColecoHW::VDP_CMD_PORT) {
            if(debug) printf("read VDP command port\n");
            data = vdp.read(1);
            return true;
        }

        if(addr == ColecoHW::VDP_DATA_PORT) {
            if(debug) printf("read VDP command port\n");
            data = vdp.read(0);
            return true;
        }


        if(addr == ColecoHW::CONTROLLER1_PORT) {
            if(debug) printf("read controller1 port\n");
            if(reading_joystick)
                data = (~user_flags & 0xFF);
            else
                data = ((~user_flags >> 8) & 0xFF);
            return true;
        }

        if(addr == ColecoHW::CONTROLLER2_PORT) {
            if(debug) printf("read controller2 port\n");
            if(reading_joystick)
                data = ((~user_flags >> 16) & 0xFF);
            else
                data = ((~user_flags >> 24) & 0xFF);
            return true;
        }

        //if(addr == ColecoHW::PIC_PORT) {
            //if(response_length == 0)
                //data = 0;
            //else {
                //data = response[response_index++];
                //if(response_index == response_length) {
                    //response_length = 0;
                    //response_index = 0;
                //}
            //}
            //return true;
        //}

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

    virtual bool nmi_requested()
    {
        return vdp.nmi_requested();
    }

    void fill_flush_audio(unsigned long long clk, audio_flush_func audio_flush)
    {
        sound.generate_audio(clk, audio_flush);
    }
};

struct RAMboard : board_base
{
    unsigned int base;
    unsigned int length;
    std::unique_ptr<unsigned char> bytes;
    RAMboard(unsigned int base_, unsigned int length_) :
        base(base_),
        length(length_),
        bytes(new unsigned char[length])
    {
    }
    virtual bool memory_read(int addr, unsigned char &data)
    {
        if(addr >= base && addr < base + length) {
            data = bytes.get()[addr - base];
            if(debug) printf("read 0x%04X -> 0x%02X from RAM\n", addr, data);
            return true;
        }
        return false;
    }
    virtual bool memory_write(int addr, unsigned char data)
    {
        if(addr >= base && addr < base + length) {
            bytes.get()[addr - base] = data;
            if(debug) printf("wrote 0x%02X to RAM 0x%04X\n", data, addr);
            return true;
        }
        return false;
    }
};

struct ROMboard : board_base
{
    unsigned int base;
    unsigned int length;
    std::unique_ptr<unsigned char> bytes;
    ROMboard(unsigned int base_, unsigned int length_, unsigned char bytes_[]) : 
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
            if(debug) printf("read 0x%04X -> 0x%02X from ROM\n", addr, data);
            return true;
        }
        return false;
    }
    virtual bool memory_write(int addr, unsigned char data)
    {
        if(addr >= base && addr < base + length) {
            if(debug) printf("attempted write 0x%02X to ROM 0x%04X ignored\n", data, addr);
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
    std::vector<BreakPoint> breakpoints;
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
        if(address < 0)
            return no_symbol;
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
        while((symbol_part - buffer) < size && (*symbol_part != ''))
            symbol_part++;
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
    Debugger()
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
        if(opcode == 0)
            break;

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
    for(int i = 0; i < insncount; i++)
	address += disassemble(address, d, 1);
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
        for(int i = 0; i <= size; i++, a++)
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
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address))
        return false;

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
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address))
        return false;

    int length = strtol(argv[2], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[2]);
        return false;
    }
    static unsigned char buffer[65536];
    for(int i = 0; i < length; i++)
        Z80_READ_BYTE(address + i, buffer[i]);
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
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address))
        return false;

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
    for(int i = 0; i < length; i++)
        Z80_WRITE_BYTE(address + i, value);
    return false;
}

bool debugger_flags(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "flags: expected flag value\n");
        return false;
    }
    char *endptr;

    user_flags = strtol(argv[1], &endptr, 0);
    if(*endptr != '\0') {
        printf("number parsing failed for %s; forgot to lead with 0x?\n", argv[2]);
        return false;
    }

    return false;
}

bool debugger_image(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    FILE *fp = fopen("output.ppm", "w");

    // XXX
    std::chrono::time_point<std::chrono::system_clock> start_time = std::chrono::system_clock::now();
    VDP->perform_scanout(framebuffer);
    std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed = now - start_time;
    if(0) printf("dump time %f seconds\n", elapsed.count());
    write_image(framebuffer, fp);
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
        clk += Z80Emulate(state, 1);
        if(i < count - 1) {
            if(verbose) {
                print_state(state);
                disassemble(state->pc, d, 1);
            }
        }
        if(d->should_debug(boards, state))
            break;
    }
    printf("%llu actual cycles emulated\n", clk);
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

    if(!lookup_or_parse(d->symbol_to_address, argv[1], state->pc))
        return false;

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
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address))
        return false;

    d->breakpoints.push_back(BreakPoint(address));
    return false;
}

bool debugger_watch(Debugger *d, std::vector<board_base*>& boards, Z80_STATE* state, int argc, char **argv)
{
    if(argc != 2) {
        fprintf(stderr, "break: expected address\n");
        return false;
    }

    int address;
    if(!lookup_or_parse(d->symbol_to_address, argv[1], address))
        return false;

    unsigned char old_value;
    Z80_READ_BYTE(address, old_value);
    d->breakpoints.push_back(BreakPoint(address, old_value));
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
    if(i < 0 || i >= d->breakpoints.size()) {
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
    if(i < 0 || i >= d->breakpoints.size()) {
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
    if(i < 0 || i >= d->breakpoints.size()) {
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
    command_handlers["flags"] = debugger_flags;
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

    for (ap = argv; (*ap = strsep(&command, " \t")) != NULL;)
        if (**ap != '\0')
            if (++ap >= &argv[10])
                break;
    int argc = ap - argv;

    if(argc == 0) {
        if(last_was_step)
            return debugger_step(this, boards, state, argc, argv);
        else
            return false;
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
        if(run)
            return true;
    }
    return false;
}

bool Debugger::should_debug(std::vector<board_base*>& boards, Z80_STATE* state)
{
    int which;
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
    for(auto b = boards.begin(); b != boards.end(); b++)
        (*b)->pause();

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
                    if(strlen(line) > 0)
                        add_history(line);
                    run = process_line(boards, state, line);
                    free(line);
                }
            } else {
                char line[512];
                if(fgets(line, sizeof(line), fp) == NULL)
                    break;
                line[strlen(line) - 1] = '\0';
                run = process_line(boards, state, line);
            }
            for(auto b = boards.begin(); b != boards.end(); b++) 
                (*b)->idle();

        } while(!run);
    }

    for(auto b = boards.begin(); b != boards.end(); b++) 
        (*b)->resume();

    previous_sigint = signal(SIGINT, mark_enter_debugger);
    state_may_have_changed = true;
}

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

const int CONTROLLER1_FIRE_BIT = 0x40;
const int CONTROLLER1_NORTH_BIT = 0x01; // 0x08;
const int CONTROLLER1_EAST_BIT = 0x02; // 0x04;
const int CONTROLLER1_SOUTH_BIT = 0x04; // 0x02;
const int CONTROLLER1_WEST_BIT = 0x08; // 0x01;
const int CONTROLLER1_KEYPAD_MASK = 0xFF00;
const int CONTROLLER1_KEYPAD_0 = 0x0500;
const int CONTROLLER1_KEYPAD_1 = 0x0200;
const int CONTROLLER1_KEYPAD_2 = 0x0800;
const int CONTROLLER1_KEYPAD_3 = 0x0300;
const int CONTROLLER1_KEYPAD_4 = 0x0D00;
const int CONTROLLER1_KEYPAD_5 = 0x0C00;
const int CONTROLLER1_KEYPAD_6 = 0x0100;
const int CONTROLLER1_KEYPAD_7 = 0x0A00;
const int CONTROLLER1_KEYPAD_8 = 0x0E00;
const int CONTROLLER1_KEYPAD_9 = 0x0400;
const int CONTROLLER1_KEYPAD_asterisk = 0x0900;
const int CONTROLLER1_KEYPAD_pound = 0x0600;

#if 0
static void handleKey(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
    if(down) {
        if(key==XK_Escape) {
            rfbCloseClient(cl);
            quit = true;
        } else if(key==XK_F12) {
            /* close down server, disconnecting clients */
            rfbShutdownServer(cl->screen,TRUE);
            quit = true;
        } else if(key==XK_F11) {
            /* close down server, but wait for all clients to disconnect */
            rfbShutdownServer(cl->screen,FALSE);
            quit = true;
        } else {
            switch(key) {
                case XK_w:
                    user_flags = (user_flags & ~CONTROLLER1_NORTH_BIT) | CONTROLLER1_NORTH_BIT;
                    break;
                case XK_a:
                    user_flags = (user_flags & ~CONTROLLER1_WEST_BIT) | CONTROLLER1_WEST_BIT;
                    break;
                case XK_s:
                    user_flags = (user_flags & ~CONTROLLER1_SOUTH_BIT) | CONTROLLER1_SOUTH_BIT;
                    break;
                case XK_d:
                    user_flags = (user_flags & ~CONTROLLER1_EAST_BIT) | CONTROLLER1_EAST_BIT;
                    break;
                case XK_space:
                    user_flags = (user_flags & ~CONTROLLER1_FIRE_BIT) | CONTROLLER1_FIRE_BIT;
                    break;
                case XK_0:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_0;
                    break;
                case XK_1:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_1;
                    break;
                case XK_2:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_2;
                    break;
                case XK_3:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_3;
                    break;
                case XK_4:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_4;
                    break;
                case XK_5:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_5;
                    break;
                case XK_6:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_6;
                    break;
                case XK_7:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_7;
                    break;
                case XK_8:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_8;
                    break;
                case XK_9:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_9;
                    break;
                case XK_asterisk:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_asterisk;
                    break;
                case XK_numbersign:
                    user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK) | CONTROLLER1_KEYPAD_pound;
                    break;
            }
        }
    } else {
        switch(key) {
            case XK_w:
                user_flags = (user_flags & ~CONTROLLER1_NORTH_BIT);
                break;
            case XK_a:
                user_flags = (user_flags & ~CONTROLLER1_WEST_BIT);
                break;
            case XK_s:
                user_flags = (user_flags & ~CONTROLLER1_SOUTH_BIT);
                break;
            case XK_d:
                user_flags = (user_flags & ~CONTROLLER1_EAST_BIT);
                break;
            case XK_space:
                user_flags = (user_flags & ~CONTROLLER1_FIRE_BIT);
                break;
            case XK_0:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_1:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_2:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_3:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_4:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_5:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_6:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_7:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_8:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_9:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_asterisk:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
            case XK_numbersign:
                user_flags = (user_flags & ~CONTROLLER1_KEYPAD_MASK);
                break;
        }
    }
}
#endif

ao_device *open_ao()
{
    ao_device *device;
    ao_sample_format format;
    int default_driver;

    ao_initialize();

    default_driver = ao_default_driver_id();

    memset(&format, 0, sizeof(format));
    format.bits = 8;
    format.channels = 1;
    format.rate = 44100;
    format.byte_format = AO_FMT_LITTLE;

    /* -- Open driver -- */
    device = ao_open_live(default_driver, &format, NULL /* no options */);
    if (device == NULL) {
        fprintf(stderr, "Error opening libao audio device.\n");
        return nullptr;
    }
    return device;
}

static GLFWwindow* my_window;

bool use_joystick = false;
int joystick_axis0 = 0;
int joystick_axis1 = 1;
int joystick_button0 = 0;
int joystick_button1 = 1;

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

#if 0
// TODO
    image_program = GenerateProgram("image", hires_vertex_shader, image_fragment_shader);
    assert(image_program != 0);
    glBindAttribLocation(image_program, raster_coords_attrib, "vertex_coords");
    CheckOpenGL(__FILE__, __LINE__);

    image_texture_location = glGetUniformLocation(image_program, "image");
    image_texture_coord_scale_location = glGetUniformLocation(image_program, "image_coord_scale");
    image_to_screen_location = glGetUniformLocation(image_program, "to_screen");
    image_x_offset_location = glGetUniformLocation(image_program, "x_offset");
    image_y_offset_location = glGetUniformLocation(image_program, "y_offset");
#endif

    // initialize_screen_areas();
    CheckOpenGL(__FILE__, __LINE__);
}

static void redraw(GLFWwindow *window)
{
    int fbw, fbh;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    glViewport(0, 0, fbw, fbh);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // upload texture
    // draw quad

    CheckOpenGL(__FILE__, __LINE__);
}

static void error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW: %s\n", description);
}

static void key(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    if(action == GLFW_PRESS || action == GLFW_REPEAT ) {
        // TODO: enqueue key press
    } else if(action == GLFW_RELEASE) {
        // TODO: enqueue key release
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

const int pixel_scale = 3;

void load_joystick_setup()
{
    FILE *fp = fopen("joystick.ini", "r");
    if(fp == NULL) {
        fprintf(stderr,"no joystick.ini file found, assuming defaults\n");
        fprintf(stderr,"store GLFW joystick axis 0 and 1 and button 0 and 1 in joystick.ini\n");
        fprintf(stderr,"e.g. \"3 4 12 11\" for Samsung EI-GP20\n");
        return;
    }
    if(fscanf(fp, "%d %d %d %d", &joystick_axis0, &joystick_axis1, &joystick_button0, &joystick_button1) != 4) {
        fprintf(stderr,"couldn't parse joystick.ini\n");
        fprintf(stderr,"store GLFW joystick axis 0 and 1 and button 0 and 1 in joystick.ini\n");
        fprintf(stderr,"e.g. \"3 4 12 11\" for Samsung EI-GP20\n");
    }
    fclose(fp);
}

void iterate_ui()
{
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

    if(glfwJoystickPresent(GLFW_JOYSTICK_1)) {
        if(false) printf("joystick 1 present\n");

        int axis_count, button_count;
        const float* axes = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &axis_count);
        const unsigned char* buttons = glfwGetJoystickButtons(GLFW_JOYSTICK_1, &button_count);

        if(false) for(int i = 0; i < axis_count; i++)
            printf("Axis %d: %f\n", i, axes[i]);
        if(false)for(int i = 0; i < button_count; i++)
            printf("Button %d: %s\n", i, (buttons[i] == GLFW_PRESS) ? "pressed" : "not pressed");

        if(axis_count <= joystick_axis0 || axis_count <= joystick_axis1) {

            fprintf(stderr, "couldn't map joystick/gamepad axes\n");
            fprintf(stderr, "mapped joystick axes are %d and %d, but maximum axis is %d\n", joystick_axis0, joystick_axis1, axis_count);
            use_joystick = false;

        } else if(button_count <= joystick_button0 && button_count <= joystick_button1) {

            fprintf(stderr, "couldn't map joystick/gamepad buttons\n");
            fprintf(stderr, "mapped buttons are %d and %d, but maximum button is %d\n", joystick_button0, joystick_button1, button_count);
            use_joystick = false;

        } else  {

#if 0
        // TODO: joystick
            paddle_values[0] = (axes[joystick_axis0] + 1) / 2;
            paddle_values[1] = (axes[joystick_axis1] + 1) / 2;

            paddle_buttons[0] = buttons[joystick_button0] == GLFW_PRESS;
            paddle_buttons[1] = buttons[joystick_button1] == GLFW_PRESS;
#endif
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

    if(!glfwInit())
        exit(EXIT_FAILURE);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); 

    // glfwWindowHint(GLFW_SAMPLES, 4);
    my_window = glfwCreateWindow(SCREEN_X * SCREEN_SCALE, SCREEN_Y * SCREEN_SCALE, "ColecoVision", NULL, NULL);
    if (!my_window) {
        glfwTerminate();
        fprintf(stdout, "Couldn't open main window\n");
        exit(EXIT_FAILURE);
    }

    glfwMakeContextCurrent(my_window);
    // printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    // printf("GL_VERSION: %s\n", glGetString(GL_VERSION));

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

int main(int argc, char **argv)
{
    Debugger *debugger = NULL;
    char *debugger_argument = NULL;

    populate_command_handlers();

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
	} else if(strcmp(argv[0], "-debugger") == 0) {
            if(argc < 2) {
                fprintf(stderr, "-debugger requires initial commands (can be empty, e.g. \"\"\n");
                usage(progname);
                exit(EXIT_FAILURE);
            }
            debugger = new Debugger();
            debugger_argument = argv[1];
	    argc -= 2;
	    argv += 2;
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

    aodev = open_ao();
    if(aodev == NULL)
        exit(EXIT_FAILURE);

    initialize_ui();

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

    audio_flush_func audio_flush;
    audio_flush = [](char *buf, size_t sz){ ao_play(aodev, buf, sz); };

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

    ColecoHW* coleco = new ColecoHW();

    boards.push_back(coleco);
    boards.push_back(bios_rom);
    boards.push_back(cart_rom);
    boards.push_back(new RAMboard(0x6000, 0x2000));

    for(auto b = boards.begin(); b != boards.end(); b++) {
        (*b)->init();
    }

    Z80_STATE state;
    memset(&state, 0, sizeof(state));

    Z80Reset(&state);

    if(debugger) {
        enter_debugger = true;
        debugger->process_line(boards, &state, debugger_argument);
    }

    std::chrono::time_point<std::chrono::system_clock> then = std::chrono::system_clock::now();

    unsigned long long clk = 0;
    while(!quit)
    {
        long long clocks_per_slice = micros_per_slice * machine_clock_rate / 1000000;

        if(debugger && (enter_debugger || debugger->should_debug(boards, &state))) {
            debugger->go(stdin, boards, &state);
            enter_debugger = false;
        } else {
            if(debugger) {
                unsigned long long cycles = 0;
                do {
                    cycles += Z80Emulate(&state, 1);
                    if(enter_debugger || debugger->should_debug(boards, &state)) {
                        debugger->go(stdin, boards, &state);
                        enter_debugger = false;
                    }
                } while(cycles < clocks_per_slice);
                clk += cycles;
            } else {
                unsigned long long cycles = 0;
                do {
                    cycles += Z80Emulate(&state, 4);
                } while(cycles < clocks_per_slice);
                clk += cycles;
            }

            VDP->perform_scanout(framebuffer);

            std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();

            auto elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(now - then);
            if(!run_fast || pause_cpu) {
                std::this_thread::sleep_for(std::chrono::microseconds(clocks_per_slice * 1000000 / machine_clock_rate) - elapsed_micros);
                // printf("%f%%\n", 100.0 - elapsed_micros.count() * 100.0 / std::chrono::microseconds(clocks_per_slice * 1000000 / machine_clock_rate).count());
            }

            then = now;
        }

        struct timeval tv;
        double start, stop;

        gettimeofday(&tv, NULL);
        start = tv.tv_sec + tv.tv_usec / 1000000.0;
        for(auto b = boards.begin(); b != boards.end(); b++) {
            int irq;
            if((*b)->board_get_interrupt(irq)) {
                // Pretend to be 8259 configured for Alice2:
                Z80_INTERRUPT_FETCH = true;
                Z80_INTERRUPT_FETCH_DATA = 0x3f00 + irq * 4;
                clk += Z80Interrupt(&state, 0xCD);
                break;
            }
        }
        gettimeofday(&tv, NULL);
        stop = tv.tv_sec + tv.tv_usec / 1000000.0;
        // printf("%f in board irq check\n", (stop - start) * 1000000);

        if(coleco->nmi_requested())
            Z80NonMaskableInterrupt (&state);

        gettimeofday(&tv, NULL);
        start = tv.tv_sec + tv.tv_usec / 1000000.0;
        for(auto b = boards.begin(); b != boards.end(); b++) {
            (*b)->idle();
        }
        gettimeofday(&tv, NULL);
        stop = tv.tv_sec + tv.tv_usec / 1000000.0;
        // printf("%f in board idle\n", (stop - start) * 1000000);

        coleco->fill_flush_audio(clk, audio_flush);
        iterate_ui();
    }

    shutdown_ui();

    return 0;
}
