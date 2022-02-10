#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <string>
#include <map>
#include <deque>
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

#include "emulator.h"
#include "z80emu.h"
#include "readhex.h"
#include "coleco_platform.h"
#include "tms9918.h"

#define PROVIDE_DEBUGGER

bool quit_requested = false;
bool enter_debugger = false; 

typedef long long clk_t;

constexpr clk_t machine_clock_rate = 3579545;
constexpr uint32_t slice_frequency = 60;
constexpr uint32_t clocks_per_slice = machine_clock_rate / slice_frequency;
constexpr clk_t micros_per_slice = 1000000 / slice_frequency;

constexpr unsigned int DEBUG_NONE = 0x00;
constexpr unsigned int DEBUG_ROM = 0x01;
constexpr unsigned int DEBUG_RAM = 0x02;
constexpr unsigned int DEBUG_IO = 0x04;
constexpr unsigned int DEBUG_SCANOUT = 0x08;
constexpr unsigned int DEBUG_VDP_OPERATIONS = 0x10;
unsigned int debug = DEBUG_NONE;
bool abort_on_exception = false;
bool do_save_images_on_vdp_write = false;
constexpr bool break_on_unknown_address = true;

Z80_STATE z80state;
bool Z80_INTERRUPT_FETCH = false;
unsigned short Z80_INTERRUPT_FETCH_DATA;


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

typedef std::function<uint8_t (const uint8_t *registers, const uint8_t *memory)> tms9918_scanout_func;
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

inline void write_rgba8_image_as_P6(uint8_t *imageRGBA, int width, int height, FILE *fp)
{
    fprintf(fp, "P6 %d %d 255\n", width, height);
    for(int row = 0; row < height; row++) {
        for(int col = 0; col < width; col++) {
            fwrite(imageRGBA + (row * width + col) * 4, 3, 1, fp);
        }
    }
}

struct TMS9918AEmulator
{
    bool cmd_started_in_nmi{false};
    int frame_number{0};
    int write_number{0};

    static constexpr int MEMORY_SIZE = 16384;
    std::array<uint8_t, MEMORY_SIZE> memory{};
    std::array<uint8_t, 8> registers{};
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
        using namespace TMS9918A;
        status_register |= VDP_STATUS_F_BIT;
    }


    void write(int cmd, unsigned char data)
    {
        using namespace TMS9918A;
        if(debug & DEBUG_VDP_OPERATIONS) printf("VDP write %d cmd==%d, in_nmi = %d\n", write_number, cmd, z80state.in_nmi);
        if(do_save_images_on_vdp_write) { /* debug */

            static unsigned char framebuffer[SCREEN_X * SCREEN_Y * 4];
            auto pixel_setter = [](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                uint8_t *pixel = framebuffer + 4 * (x + y * SCREEN_X) + 0;
                SetColor(pixel, r, g, b);
            };

            CreateImageAndReturnFlags(registers.data(), memory.data(), pixel_setter);
            char name[512];
            sprintf(name, "frame_%04d_%05d_%d_%02X.ppm", frame_number, write_number, cmd, data);
            FILE *fp = fopen(name, "wb");
            write_rgba8_image_as_P6(framebuffer, SCREEN_X, SCREEN_Y, fp);
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
        using namespace TMS9918A;
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

    void perform_scanout(tms9918_scanout_func scanout)
    {
        frame_number++;
        write_number = 0;
        if(debug & DEBUG_SCANOUT) {
            printf("scanout frame %d\n", frame_number);
        }

        status_register |= scanout(registers.data(), memory.data());
    }

    bool nmi_required()
    {
        using namespace TMS9918A;
        return InterruptsAreEnabled(registers.data()) && VSyncInterruptHasOccurred(status_register);
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
                data = PlatformInterface::GetJoystickState(PlatformInterface::CONTROLLER_1);
            } else {
                data = PlatformInterface::GetKeypadState(PlatformInterface::CONTROLLER_1);
            }
            if(debug & DEBUG_IO) printf("read controller1 port 0x%02X, read 0x%02X\n", addr, data);
#ifdef PROVIDE_DEBUGGER
            io_reads.insert(addr);
#endif
            return true;
        }

        if((addr >= 0xE0) && (addr <= 0xFF) && ((addr & 0x02) == 0x2)) {
            if(reading_joystick) {
                data = PlatformInterface::GetJoystickState(PlatformInterface::CONTROLLER_2);
            } else {
                data = PlatformInterface::GetKeypadState(PlatformInterface::CONTROLLER_2);
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
    using namespace TMS9918A;
    FILE *fp = fopen("output.ppm", "wb");

    auto& vdp = d->colecohw->vdp;

    // XXX
    static unsigned char framebuffer[SCREEN_X * SCREEN_Y * 4];
    auto pixel_setter = [](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t *pixel = framebuffer + 4 * (x + y * SCREEN_X) + 0;
        SetColor(pixel, r, g, b);
    };
    std::chrono::time_point<std::chrono::system_clock> start_time = std::chrono::system_clock::now();
    CreateImageAndReturnFlags(vdp.registers.data(), vdp.memory.data(), pixel_setter);
    std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed = now - start_time;
    if(false) printf("dump time %f seconds\n", elapsed.count());
    write_rgba8_image_as_P6(framebuffer, SCREEN_X, SCREEN_Y, fp);
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
    quit_requested = true;
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
                    quit_requested = true;
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

#else

struct Debugger
{
    Debugger(ColecoHW *colecohw, clk_t& clk) {}
}

#endif

constexpr bool profiling = false;

volatile bool run_fast = false;
volatile bool pause_cpu = false;

std::vector<board_base*> boards;

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

using namespace PlatformInterface;


void do_vdp_test(const char *vdp_dump_name, const char *image_name)
{
    using namespace TMS9918A;
    std::array<uint8_t, 8> registers;
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
        SetColor(pixel, r, g, b);
    };
    CreateImageAndReturnFlags(registers.data(), memory.data(), pixel_setter);
    FILE *fp = fopen(image_name, "wb");
    write_rgba8_image_as_P6(framebuffer, SCREEN_X, SCREEN_Y, fp);
    fclose(fp);
}

void WriteVDPStateToFile(const char *base, int which, const uint8_t* registers, const uint8_t *memory, FILE *vdp_file)
{
    fprintf(vdp_file, "# %s_%02d.vdp, 8 register bytes, 16384 RAM bytes\n", base, which);
    for(size_t i = 0; i < 8; i++) {
        bool more_in_row = ((i + 1) % 8) != 0;
        fprintf(vdp_file, "%u%s", registers[i], more_in_row ? " " : "\n");
    }
    fputs("", vdp_file);
    for(size_t i = 0; i < 16384; i++) {
        bool more_in_row = ((i + 1) % 16) != 0;
        fprintf(vdp_file, "%u%s", memory[i], more_in_row ? " " : "\n");
    }
    fputs("", vdp_file);
}

void SaveVDPState(const TMS9918AEmulator *vdp, int which)
{
    static char filename[512];
    sprintf(filename, "%s_%02d.vdp", getenv("VDP_OUT_BASE"), which);
    FILE *vdp_file = fopen(filename, "w");
    WriteVDPStateToFile(getenv("VDP_OUT_BASE"), which, vdp->registers.data(), vdp->memory.data(), vdp_file);
    fclose(vdp_file);
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

    PlatformInterface::Start();

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

    audio_flush_func audio_flush = [](uint8_t *buf, size_t sz){ PlatformInterface::EnqueueAudioSamples(buf, sz); };

    tms9918_scanout_func platform_scanout = [](const uint8_t *registers, const uint8_t *memory)->uint8_t {
        uint8_t status_result;
        PlatformInterface::Frame(registers, memory, status_result, 3.579f);
        return status_result;
    };

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
    ColecoHW* colecohw = new ColecoHW(PlatformInterface::GetAudioSampleRate(), PlatformInterface::GetPreferredAudioBufferSampleCount());
    bool save_vdp = false;

    Debugger *debugger = NULL;
#ifdef PROVIDE_DEBUGGER
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

    PlatformInterface::MainLoopBodyFunc main_loop_body = [&clk, debugger, colecohw, &nmi_was_issued, &save_vdp, audio_flush, platform_scanout, &then]() {

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
                        colecohw->vdp.perform_scanout(platform_scanout);
                        if(save_vdp) {
                            static int which = 0;
                            SaveVDPState(&colecohw->vdp, which++);
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
        while(PlatformInterface::EventIsWaiting()) {
            PlatformInterface::Event e = PlatformInterface::DequeueEvent();
            if(e.type == PlatformInterface::QUIT) {
                quit_requested = true;
            } else if(e.type == PlatformInterface::RESET) {
                Z80Reset(&z80state);
            } else if(e.type == PlatformInterface::SAVE_VDP_STATE) {
                save_vdp = true;
            } else if(e.type == PlatformInterface::DEBUG_VDP_WRITES) {
                do_save_images_on_vdp_write = !do_save_images_on_vdp_write;
            } else {
                printf("warning: unhandled platform event type %d\n", e.type);
            }
        }
        after = std::chrono::system_clock::now();
        real_elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(after - before);
        if(profiling) printf("UI %lld\n", real_elapsed_micros.count());

        return quit_requested;
    };

    PlatformInterface::MainLoopAndShutdown(main_loop_body);

    return 0;
}
