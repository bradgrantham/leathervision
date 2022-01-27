#include <tuple>
#include <vector>

namespace COLECOinterface
{

enum EventType
{
    NONE, RESET, SPEED, QUIT, PAUSE,
};

struct event {
    EventType type;
    int value;
    char *str;
    event(EventType type_, int value_, char *str_ = NULL) :
        type(type_),
        value(value_),
        str(str_)
    {}
};

bool event_waiting();
event dequeue_event();

enum ControllerIndex { CONTROLLER_1, CONTROLLER_2 };
uint8_t GetJoystickState(ControllerIndex controller);
uint8_t GetKeypadState(ControllerIndex controller);

int get_audio_sample_rate();
size_t get_preferred_audio_buffer_size_samples();
void enqueue_audio_samples(uint8_t *buf, size_t sz);

void start();
void iterate(const std::array<uint8_t, 64>& vdp_registers, const std::array<uint8_t, 16384>& vdp_ram, uint8_t& vdp_status_result, float megahertz); // update display
void shutdown();

};
