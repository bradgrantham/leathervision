#include <deque>
#include <chrono>

#include "coleco_platform.h"

#include <ao/ao.h>

#if defined(__linux__)
#include <GL/glew.h>
#endif // defined(__linux__)

#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

#ifdef __linux__
#include <linux/i2c-dev.h>
#endif

#include "gl_utility.h"
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

void EnqueueAudioSamples(uint8_t *buf, size_t sz)
{
    ao_play(aodev, (char*)buf, sz);
}

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

unsigned char framebuffer[TMS9918A::SCREEN_X * TMS9918A::SCREEN_Y * 4];

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

    screen_image = initialize_texture(TMS9918A::SCREEN_X, TMS9918A::SCREEN_Y, NULL);
    screen_image_rectangle.push_back({make_rectangle_array_buffer(0, 0, TMS9918A::SCREEN_X, TMS9918A::SCREEN_Y), raster_coords_attrib, 2, GL_FLOAT, GL_FALSE, 0});
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TMS9918A::SCREEN_X, TMS9918A::SCREEN_Y, 0, GL_RGBA, GL_UNSIGNED_BYTE, framebuffer);
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
            case GLFW_KEY_Z:
                event_queue.push_back({DUMP_SOME_AUDIO, 0});
                break;
            case GLFW_KEY_V:
                event_queue.push_back({SAVE_VDP_STATE, 0});
                break;
            case GLFW_KEY_N:
                event_queue.push_back({DEBUG_VDP_WRITES, 0});
                break;
            case GLFW_KEY_R:
                event_queue.push_back({RESET, 0});
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
        event_queue.push_back({QUIT, 0});
        return;
    }

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

constexpr int SCREEN_SCALE = 3;

std::chrono::time_point<std::chrono::system_clock> previous_draw_time;
std::chrono::time_point<std::chrono::system_clock> previous_event_time;

void Start(int& audioSampleRate, size_t& preferredAudioBufferSampleCount)
{
    aodev = open_ao(audio_rate);
    if(aodev == NULL)
        exit(EXIT_FAILURE);
    audioSampleRate = audio_rate;
    preferredAudioBufferSampleCount = audio_rate / 100;

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
    my_window = glfwCreateWindow(TMS9918A::SCREEN_X * SCREEN_SCALE, TMS9918A::SCREEN_Y * SCREEN_SCALE, "ColecoVision", NULL, NULL);
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

#ifdef __linux__
    if(!cvhat_init()) {
	printf("couldn't connect to colecovision controller HAT.\n");
    }
#endif
     previous_event_time = previous_draw_time = std::chrono::system_clock::now();
}

void Frame(const uint8_t* vdp_registers, const uint8_t* vdp_ram, uint8_t& vdp_status_result, [[maybe_unused]] float megahertz)
{
    auto pixel_setter = [](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t *pixel = framebuffer + 4 * (x + y * TMS9918A::SCREEN_X) + 0;
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
    };

    vdp_status_result = TMS9918A::CreateImageAndReturnFlags(vdp_registers, vdp_ram, pixel_setter);

    std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
    std::chrono::duration<float> elapsed;
    
    elapsed = now - previous_event_time;
    if(elapsed.count() > .02) {
        iterate_ui();
        previous_event_time = now;
    }

    elapsed = now - previous_draw_time;
    if(elapsed.count() > .02)
    {
        CheckOpenGL(__FILE__, __LINE__);
        redraw(my_window);
        CheckOpenGL(__FILE__, __LINE__);
        glfwSwapBuffers(my_window);
        CheckOpenGL(__FILE__, __LINE__);
        previous_draw_time = now;
    }
}

void MainLoopAndShutdown(MainLoopBodyFunc body)
{
    bool quit_requested = false;
    while(!quit_requested)
    {
        iterate_ui();
        quit_requested = body();
    }

    glfwTerminate();
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
#error need to add cvhat_read_controllers(); to event loop

#endif

};


