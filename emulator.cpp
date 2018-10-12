#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <map>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "emulator.h"

#include "z80emu.h"
#include "readhex.h"

const bool debug = false;

bool Z80_INTERRUPT_FETCH = false;
unsigned short Z80_INTERRUPT_FETCH_DATA;

std::vector<board_base*> boards;

bool quit = false;

struct ColecoHW : board_base
{
    bool debug;

    bool reading_joystick = true;

    // static const int PIC_PORT = 0;

    static const int VDP_CTRL_PORT = 0xBF;

    static const int SN76489A_PORT = 0xFF;

    static const int SWITCH_TO_KEYPAD_PORT = 0x80;
    static const int SWITCH_TO_JOYSTICK_PORT = 0xC0;
    static const int CONTROLLER1_PORT = 0xFC;
    static const int CONTROLLER2_PORT = 0xFF;

    enum {VDP_PHASE_VALUE, VDP_PHASE_REG} vdp_phase = VDP_PHASE_VALUE;

    ColecoHW()
    {
        debug = true;
    }

    virtual bool io_write(int addr, unsigned char data)
    {
        // if(addr == ColecoHW::PROPELLER_PORT) {
            // write_to_propeller(data);
            // return true;
        // }

        if(addr == ColecoHW::VDP_CTRL_PORT) {
            if(vdp_phase == VDP_PHASE_VALUE) {
                if(debug) printf("VDP control write value 0x%02X\n", data);
            } else {
                if(debug) printf("VDP control write register 0x%02X\n", data);
            }
            vdp_phase = (vdp_phase == VDP_PHASE_VALUE) ? VDP_PHASE_REG : VDP_PHASE_VALUE;
            return true;
        }

        if(addr == ColecoHW::SN76489A_PORT) {
            if(debug) printf("audio write 0x%02X\n", data);
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
        if(addr == ColecoHW::CONTROLLER1_PORT) {
            if(debug) printf("read controller1 port\n");
            data = 0;
            return true;
        }
        if(addr == ColecoHW::CONTROLLER2_PORT) {
            if(debug) printf("read controller2 port\n");
            data = 0;
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
    unsigned long long total_cycles = 0;
    for(int i = 0; i < count; i++) {
        total_cycles += Z80Emulate(state, 1);
        if(i < count - 1) {
            if(verbose) {
                print_state(state);
                disassemble(state->pc, d, 1);
            }
        }
        if(d->should_debug(boards, state))
            break;
    }
    printf("%llu actual cycles emulated\n", total_cycles);
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

int main(int argc, char **argv)
{
    const int cycles_per_loop = 50000;

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

    static unsigned char rom_temp[65536];
    FILE *fp;

    char *bios_name = argv[0];
    char *cart_name = argv[0];

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

    boards.push_back(new ColecoHW());
    boards.push_back(bios_rom);
    boards.push_back(cart_rom);
    boards.push_back(new RAMboard(0x6000, 0x2000));

    for(auto b = boards.begin(); b != boards.end(); b++) {
        (*b)->init();
    }

    Z80_STATE state;

    Z80Reset(&state);

    if(debugger) {
        enter_debugger = true;
        debugger->process_line(boards, &state, debugger_argument);
    }

    time_t time_then;
    time_then = time(NULL);
    unsigned long long total_cycles = 0;
    unsigned long long cycles_then = 0;
    while(!quit)
    {
        if(debugger && (enter_debugger || debugger->should_debug(boards, &state))) {
            debugger->go(stdin, boards, &state);
            enter_debugger = false;
        } else {
            struct timeval tv;
            double start, stop;

            gettimeofday(&tv, NULL);
            start = tv.tv_sec + tv.tv_usec / 1000000.0;
            if(debugger) {
                unsigned long long cycles = 0;
                do {
                    cycles += Z80Emulate(&state, 1);
                    if(enter_debugger || debugger->should_debug(boards, &state)) {
                        debugger->go(stdin, boards, &state);
                        enter_debugger = false;
                    }
                } while(cycles < cycles_per_loop);
                total_cycles += cycles;
            } else {
                total_cycles += Z80Emulate(&state, cycles_per_loop);
            }
            time_t then;
            then = time(NULL);

            time_t time_now;
            time_now = time(NULL);
            if(time_now != time_then) {
                if(debug) printf("%llu cycles per second\n", total_cycles - cycles_then);
                cycles_then = total_cycles;
                time_then = time_now;
            }
            gettimeofday(&tv, NULL);
            stop = tv.tv_sec + tv.tv_usec / 1000000.0;
            // printf("%f in Emulate\n", (stop - start) * 1000000);
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
                total_cycles += Z80Interrupt(&state, 0xCD);
                break;
            }
        }
        gettimeofday(&tv, NULL);
        stop = tv.tv_sec + tv.tv_usec / 1000000.0;
        // printf("%f in board irq check\n", (stop - start) * 1000000);

        gettimeofday(&tv, NULL);
        start = tv.tv_sec + tv.tv_usec / 1000000.0;
        for(auto b = boards.begin(); b != boards.end(); b++) {
            (*b)->idle();
        }
        gettimeofday(&tv, NULL);
        stop = tv.tv_sec + tv.tv_usec / 1000000.0;
        // printf("%f in board idle\n", (stop - start) * 1000000);
    }

    return 0;
}
