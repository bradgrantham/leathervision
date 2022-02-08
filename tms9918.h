#include <cstdint>
#include <cstdio>
#include <array>

namespace TMS9918A
{

constexpr int SCREEN_X = 256;
constexpr int SCREEN_Y = 192;

constexpr int REG_A0_A5_MASK = 0x3F;
constexpr int CMD_MASK = 0xC0;
constexpr int CMD_SET_REGISTER = 0x80;
constexpr int CMD_SET_WRITE_ADDRESS = 0x40;
constexpr int CMD_SET_READ_ADDRESS = 0x00;

constexpr int VR0_M3_MASK = 0x02;
[[maybe_unused]] constexpr int VR0_EXTVID_MASK = 0x01;

[[maybe_unused]] constexpr int VR1_16K_MASK = 0x80; 
constexpr int VR1_BLANK_MASK = 0x40; /* and BLANK is active low */
constexpr int VR1_INT_MASK = 0x20;
constexpr int VR1_M2_MASK = 0x10;
constexpr int VR1_M1_MASK = 0x08;
constexpr int VR1_SIZE4_MASK = 0x02;
constexpr int VR1_MAG2X_MASK = 0x01;

constexpr int VR2_NAME_TABLE_MASK = 0x0F;
constexpr int VR2_NAME_TABLE_SHIFT = 10;

constexpr int VR3_COLORTABLE_MASK_STANDARD = 0xFF;
constexpr int VR3_COLORTABLE_SHIFT_STANDARD = 6;

constexpr int VR3_COLORTABLE_MASK_BITMAP = 0x80;
constexpr int VR3_COLORTABLE_SHIFT_BITMAP = 6;

constexpr int VR3_ADDRESS_MASK_BITMAP = 0x7F;
constexpr int VR3_ADDRESS_MASK_SHIFT = 6;

constexpr int VR4_PATTERN_MASK_STANDARD = 0x07;
constexpr int VR4_PATTERN_SHIFT_STANDARD = 11;

constexpr int VR4_PATTERN_MASK_BITMAP = 0x04;
constexpr int VR4_PATTERN_SHIFT_BITMAP = 11;

constexpr int VR5_SPRITE_ATTR_MASK = 0x7F;
constexpr int VR5_SPRITE_ATTR_SHIFT = 7;

constexpr int VR6_SPRITE_PATTERN_MASK = 0x07;
constexpr int VR6_SPRITE_PATTERN_SHIFT = 11;

constexpr int VR7_BD_MASK = 0x0F;
constexpr int VR7_BD_SHIFT = 0;

constexpr int VDP_STATUS_F_BIT = 0x80;
constexpr int VDP_STATUS_5S_BIT = 0x40;
constexpr int VDP_STATUS_C_BIT = 0x20;

constexpr int ROW_SHIFT = 5;
constexpr int THIRD_SHIFT = 11;
constexpr int CHARACTER_PATTERN_SHIFT = 3;
constexpr int CHARACTER_COLOR_SHIFT = 3;
constexpr int ADDRESS_MASK_FILL = 0x3F;

constexpr int SPRITE_EARLY_CLOCK_MASK = 0x80;
constexpr int SPRITE_COLOR_MASK = 0x0F;
constexpr int SPRITE_NAME_SHIFT = 3;
constexpr int SPRITE_NAME_MASK_SIZE4 = 0xFC;

constexpr int TRANSPARENT_COLOR_INDEX = 0;

constexpr int REGISTER_COUNT = 8;

typedef std::array<uint8_t, REGISTER_COUNT> register_file_t;

enum GraphicsMode { GRAPHICS_I, GRAPHICS_II, TEXT, MULTICOLOR, UNDEFINED };

bool SpritesAreSize4(const register_file_t& registers)
{
    return registers[1] & VR1_SIZE4_MASK;
}

bool SpritesAreMagnified2X(const register_file_t& registers)
{
    return registers[1] & VR1_MAG2X_MASK;
}

bool ActiveDisplayAreaIsBlanked(const register_file_t& registers)
{
    return (registers[1] & VR1_BLANK_MASK) == 0;
}

uint8_t GetBackdropColor(const register_file_t& registers)
{
    return (registers[7] & VR7_BD_MASK) >> VR7_BD_SHIFT;
}

bool InterruptsAreEnabled(const register_file_t& registers)
{
    return registers[1] & VR1_INT_MASK;
}

bool VSyncInterruptHasOccurred(uint8_t status_register)
{
    return status_register & VDP_STATUS_F_BIT;
}

GraphicsMode GetGraphicsMode(const register_file_t& registers)
{
    bool M1 = registers[1] & VR1_M1_MASK;
    bool M2 = registers[1] & VR1_M2_MASK;
    bool M3 = registers[0] & VR0_M3_MASK;

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
    return (registers[2] & VR2_NAME_TABLE_MASK) << VR2_NAME_TABLE_SHIFT;
}

uint16_t GetSpriteAttributeTableBase(const register_file_t& registers)
{
    return (registers[5] & VR5_SPRITE_ATTR_MASK) << VR5_SPRITE_ATTR_SHIFT;
}

uint16_t GetSpritePatternTableBase(const register_file_t& registers)
{
    return (registers[6] & VR6_SPRITE_PATTERN_MASK) << VR6_SPRITE_PATTERN_SHIFT;
}

};


