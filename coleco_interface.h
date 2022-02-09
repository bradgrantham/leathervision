#include <tuple>
#include <vector>

namespace PlatformInterface
{

enum EventType
{
    NONE, RESET, SPEED, QUIT, PAUSE,
};

struct Event {
    EventType type;
    int value;
    char *str; // ownership transfered - caller of DequeueEvent must free
    Event(EventType type, int value, char *str = NULL) :
        type(type),
        value(value),
        str(str)
    {}
};

bool EventIsWaiting();
Event DequeueEvent();

enum ControllerIndex { CONTROLLER_1, CONTROLLER_2 };
uint8_t GetJoystickState(ControllerIndex controller);
uint8_t GetKeypadState(ControllerIndex controller);

int GetAudioSampleRate();
size_t GetPreferredAudioBufferSampleCount();
void EnqueueAudioSamples(uint8_t *buf, size_t sz);

void Start();
void Frame(const uint8_t* vdp_registers, const uint8_t* vdp_ram, uint8_t& vdp_status_result, float megahertz);  // update display and block to retrace
void Shutdown();

};
