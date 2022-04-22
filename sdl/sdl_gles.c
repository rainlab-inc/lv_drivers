/**
 * @file sdl_gles.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "sdl_gles.h"

#if USE_SDL_GLES
#if LV_USE_GPU_GLES == 0
# error "LV_USE_GPU_GLES must be enabled"
#endif


#include LV_GPU_GLES_EPOXY_INCLUDE_PATH
#include SDL_INCLUDE_PATH

/*********************
 *      DEFINES
 *********************/
#ifndef KEYBOARD_BUFFER_SIZE
#define KEYBOARD_BUFFER_SIZE SDL_TEXTINPUTEVENT_TEXT_SIZE
#endif
#if LV_USE_GPU_GLES_SW_MIXED
    #define BYTES_PER_PIXEL 3
#endif
/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    SDL_Window *window;
    SDL_GLContext *context;

    GLuint program;
    GLint position_location;
    GLint uv_location;

    GLuint texture;

#if LV_USE_GPU_GLES_SW_MIXED
    GLubyte *texture_pixels;
#else
    GLuint framebuffer;
#endif /* LV_USE_GPU_GLES_SW_MIXED */
    volatile bool sdl_refr_qry;
}monitor_t;


/**********************
 *  STATIC PROTOTYPES
 **********************/
static void window_create(monitor_t * m);
static void window_update(monitor_t * m);
static void monitor_sdl_gles_clean_up(void);
static void sdl_gles_event_handler(lv_timer_t * t);
static void monitor_sdl_gles_refr(lv_timer_t * t);
static void mouse_handler(SDL_Event *event);
static void keyboard_handler(SDL_Event * event);
static uint32_t keycode_to_ctrl_key(SDL_Keycode sdl_key);
static int tick_thread(void *data);
static GLuint gl_shader_program_create(const char *vertex_src, const char *fragment_src);
#if LV_USE_GPU_GLES_SW_MIXED
static GLuint gl_texture_create(int width, int height, GLubyte *pixels);

// From the article https://lupyuen.github.io/pinetime-rust-mynewt/articles/wayland
static void put_px(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
#endif /* LV_USE_GPU_GLES_SW_MIXED */

/**********************
 *  STATIC VARIABLES
 **********************/
static monitor_t monitor;
static volatile bool quit = false;

static bool left_button_down = false;
static int16_t last_x = 0;
static int16_t last_y = 0;

static char buf[KEYBOARD_BUFFER_SIZE];


static char vertex_shader_str[] =
    "attribute vec2 a_position;   \n"
    "attribute vec2 a_texcoord;   \n"
    "varying vec2 v_texcoord;     \n"
    "void main()                  \n"
    "{                            \n"
    "   gl_Position = vec4(a_position.x, a_position.y, 0.0, 1.0); \n"
    "   v_texcoord = a_texcoord;  \n"
    "}                            \n";

static char fragment_shader_str[] =
    "precision mediump float;                            \n"
    "varying vec2 v_texcoord;                            \n"
    "uniform sampler2D s_texture;                        \n"
    "void main()                                         \n"
    "{                                                   \n"
    "  gl_FragColor = texture2D(s_texture, v_texcoord );\n"
    "}                                                   \n";


static GLfloat vertices[] = {
#if LV_USE_GPU_GLES_SW_MIXED
    -1.0f,  1.0f,  0.0f, 0.0f,
    -1.0f, -1.0f,  0.0f, 1.0f,
    1.0f, -1.0f,  1.0f, 1.0f,

    -1.0f,  1.0f,  0.0f, 0.0f,
    1.0f, -1.0f,  1.0f, 1.0f,
    1.0f,  1.0f,  1.0f, 0.0f
#else
    -1.0f,  1.0f,  0.0f, 1.0f,
    -1.0f, -1.0f,  0.0f, 0.0f,
    1.0f, -1.0f,  1.0f, 0.0f,

    -1.0f,  1.0f,  0.0f, 1.0f,
    1.0f, -1.0f,  1.0f, 0.0f,
    1.0f,  1.0f,  1.0f, 1.0f
#endif /* LV_USE_GPU_GLES_SW_MIXED */
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void sdl_gles_init(void)
{
    SDL_Init(SDL_INIT_VIDEO);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    window_create(&monitor);

    SDL_CreateThread(tick_thread, "tick", NULL);

    lv_timer_create(sdl_gles_event_handler, 10, NULL);
}

void sdl_gles_disp_draw_buf_init(lv_disp_draw_buf_t *draw_buf)
{
#if LV_USE_GPU_GLES_SW_MIXED
    static lv_color_t buf1_1[SDL_HOR_RES * SDL_VER_RES];
    lv_disp_draw_buf_init(draw_buf, buf1_1, NULL, SDL_HOR_RES * SDL_VER_RES);
#else
    lv_disp_draw_buf_init(draw_buf, NULL, NULL, SDL_HOR_RES * SDL_VER_RES);
#endif /* LV_USE_GPU_GLES_SW_MIXED */
}

void sdl_gles_disp_drv_init(lv_disp_drv_t *driver, lv_disp_draw_buf_t *draw_buf)
{
    lv_disp_drv_init(driver);
    driver->draw_buf = draw_buf;
    driver->flush_cb = sdl_gles_display_flush;
    driver->hor_res = SDL_HOR_RES;
    driver->ver_res = SDL_VER_RES;
    driver->direct_mode = 1;
    driver->full_refresh = 1;
#if !LV_USE_GPU_GLES_SW_MIXED
    driver->user_data = &monitor.framebuffer;
#endif /* !LV_USE_GPU_GLES_SW_MIXED */
}


// From the article https://lupyuen.github.io/pinetime-rust-mynewt/articles/wayland
void sdl_gles_display_flush(lv_disp_drv_t * disp_drv,
                            const lv_area_t * area, lv_color_t * color_p)
{
#if LV_USE_GPU_GLES_SW_MIXED
    int32_t x;
    int32_t y;
    for (y = area->y1; y <= area->y2; y++) {
        for (x = area->x1; x <= area->x2; x++) {
            /* Put a pixel to the display */
            put_px(x, y,
                   color_p->ch.red,
                   color_p->ch.green,
                   color_p->ch.blue,
                   0xff);
            color_p++;
        }
    }
    glBindTexture(GL_TEXTURE_2D, monitor.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SDL_HOR_RES, SDL_VER_RES, GL_RGB, GL_UNSIGNED_BYTE, monitor.texture_pixels);
    monitor.sdl_refr_qry = true;
    monitor_sdl_gles_refr(NULL);
    lv_disp_flush_ready(disp_drv);
#else
    lv_coord_t hres = disp_drv->hor_res;
    lv_coord_t vres = disp_drv->ver_res;


    /*Return if the area is out the screen*/
    if(area->x2 < 0 || area->y2 < 0 || area->x1 > hres - 1 || area->y1 > vres - 1) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    monitor.sdl_refr_qry = true;

    /* TYPICALLY YOU DO NOT NEED THIS
     * If it was the last part to refresh update the texture of the window.*/
    if(lv_disp_flush_is_last(disp_drv)) {
        monitor_sdl_gles_refr(NULL);
    }

    /*IMPORTANT! It must be called to tell the system the flush is ready*/
    lv_disp_flush_ready(disp_drv);
#endif /* LV_USE_GPU_GLES_SW_MIXED */
}

void sdl_gles_mouse_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    (void) indev_drv;

    data->point.x = last_x;
    data->point.y = last_y;

    if (left_button_down) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

}

void sdl_gles_keyboard_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    (void) indev_drv;      /*Unused*/

    static bool dummy_read = false;
    const size_t len = strlen(buf);

    /*Send a release manually*/
    if (dummy_read) {
        dummy_read = false;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = len > 0;
    }
        /*Send the pressed character*/
    else if (len > 0) {
        dummy_read = true;
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = buf[0];
        memmove(buf, buf + 1, len);
        data->continue_reading = true;
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void sdl_gles_event_handler(lv_timer_t * t)
{
    (void)t;

    SDL_Event event;
        while (SDL_PollEvent(&event)) {
            mouse_handler(&event);
            keyboard_handler(&event);

            if (event.type == SDL_WINDOWEVENT) {
                switch ((&event)->window.event) {
#if SDL_VERSION_ATLEAST(2, 0, 5)
                    case SDL_WINDOWEVENT_TAKE_FOCUS:
#endif
                    case SDL_WINDOWEVENT_EXPOSED:
                        window_update(&monitor);
                        break;
                        /* TODO: handle dual display */
                    default:
                        break;
                }
            }
            if (event.type == SDL_QUIT) {
                quit = true;
            }
        }


    if (quit) {
        monitor_sdl_gles_clean_up();
        exit(EXIT_SUCCESS);
    }

}

static void window_create(monitor_t *m)
{
    m->window = SDL_CreateWindow("lvgl-opengl",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_HOR_RES * SDL_ZOOM, SDL_VER_RES * SDL_ZOOM,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);

    m->context = SDL_GL_CreateContext(m->window);


    printf( "GL version : %s\n", glGetString(GL_VERSION));
    printf( "GL vendor : %s\n", glGetString(GL_VENDOR));
    printf( "GL renderer : %s\n", glGetString(GL_RENDERER));
    fflush(stdout);

    m->program = gl_shader_program_create(vertex_shader_str, fragment_shader_str);

    glUseProgram(m->program);
    m->position_location = glGetAttribLocation(m->program, "a_position");
    m->uv_location = glGetAttribLocation(m->program, "a_texcoord");

    glGenTextures(1, &m->texture);
    glBindTexture(GL_TEXTURE_2D, m->texture);
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

#if LV_USE_GPU_GLES_SW_MIXED
    m->texture_pixels = malloc(SDL_HOR_RES * SDL_VER_RES * BYTES_PER_PIXEL * sizeof(GLubyte));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, SDL_HOR_RES, SDL_VER_RES, 0, GL_RGB, GL_UNSIGNED_BYTE, m->texture_pixels);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SDL_HOR_RES, SDL_VER_RES, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &m->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m->texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif /* LV_USE_GPU_GLES_SW_MIXED */

    glBindTexture(GL_TEXTURE_2D, 0);
    m->sdl_refr_qry = true;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

}

static void monitor_sdl_gles_refr(lv_timer_t *t)
{
    (void)t;
    /*Refresh handling*/
    if(monitor.sdl_refr_qry != false) {
        monitor.sdl_refr_qry = false;
        window_update(&monitor);
    }
}

static void window_update(monitor_t *m)
{

    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);


    glUseProgram(m->program);
    glBindTexture(GL_TEXTURE_2D, m->texture);

    glVertexAttribPointer(m->position_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices);
    glEnableVertexAttribArray(m->position_location);
    glVertexAttribPointer(m->uv_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &vertices[2]);
    glEnableVertexAttribArray(m->uv_location);
    glDrawArrays(GL_TRIANGLES, 0, 6);


    SDL_GL_SwapWindow(m->window);

}

static void mouse_handler(SDL_Event *event)
{
    switch(event->type) {
        case SDL_MOUSEBUTTONUP:
            if(event->button.button == SDL_BUTTON_LEFT)
                left_button_down = false;
            break;
        case SDL_MOUSEBUTTONDOWN:
            if(event->button.button == SDL_BUTTON_LEFT) {
                left_button_down = true;
                last_x = event->motion.x / SDL_ZOOM;
                last_y = event->motion.y / SDL_ZOOM;
            }
            break;
        case SDL_MOUSEMOTION:
            last_x = event->motion.x / SDL_ZOOM;
            last_y = event->motion.y / SDL_ZOOM;
            break;

        case SDL_FINGERUP:
            left_button_down = false;
            last_x = LV_HOR_RES * event->tfinger.x / SDL_ZOOM;
            last_y = LV_VER_RES * event->tfinger.y / SDL_ZOOM;
            break;
        case SDL_FINGERDOWN:
            left_button_down = true;
            last_x = LV_HOR_RES * event->tfinger.x / SDL_ZOOM;
            last_y = LV_VER_RES * event->tfinger.y / SDL_ZOOM;
            break;
        case SDL_FINGERMOTION:
            last_x = LV_HOR_RES * event->tfinger.x / SDL_ZOOM;
            last_y = LV_VER_RES * event->tfinger.y / SDL_ZOOM;
            break;
    }
}

static void keyboard_handler(SDL_Event * event)
{
    /* We only care about SDL_KEYDOWN and SDL_TEXTINPUT events */
    switch(event->type) {
        case SDL_KEYDOWN:                       /*Button press*/
        {
            const uint32_t ctrl_key = keycode_to_ctrl_key(event->key.keysym.sym);
            if (ctrl_key == '\0')
                return;
            const size_t len = strlen(buf);
            if (len < KEYBOARD_BUFFER_SIZE - 1) {
                buf[len] = ctrl_key;
                buf[len + 1] = '\0';
            }
            break;
        }
        case SDL_TEXTINPUT:                     /*Text input*/
        {
            const size_t len = strlen(buf) + strlen(event->text.text);
            if (len < KEYBOARD_BUFFER_SIZE - 1)
                strcat(buf, event->text.text);
        }
            break;
        default:
            break;

    }
}



static uint32_t keycode_to_ctrl_key(SDL_Keycode sdl_key)
{
    /*Remap some key to LV_KEY_... to manage groups*/
    switch(sdl_key) {
        case SDLK_RIGHT:
        case SDLK_KP_PLUS:
            return LV_KEY_RIGHT;

        case SDLK_LEFT:
        case SDLK_KP_MINUS:
            return LV_KEY_LEFT;

        case SDLK_UP:
            return LV_KEY_UP;

        case SDLK_DOWN:
            return LV_KEY_DOWN;

        case SDLK_ESCAPE:
            return LV_KEY_ESC;

        case SDLK_BACKSPACE:
            return LV_KEY_BACKSPACE;

        case SDLK_DELETE:
            return LV_KEY_DEL;

        case SDLK_KP_ENTER:
        case '\r':
            return LV_KEY_ENTER;

        case SDLK_TAB:
        case SDLK_PAGEDOWN:
            return LV_KEY_NEXT;

        case SDLK_PAGEUP:
            return LV_KEY_PREV;

        default:
            return '\0';
    }
}


static void monitor_sdl_gles_clean_up(void)
{
    SDL_DestroyWindow(monitor.window);
}

static int tick_thread(void *data)
{
    (void)data;

    while(!quit) {
        SDL_Delay(5);
        lv_tick_inc(5); /*Tell LittelvGL that 5 milliseconds were elapsed*/
    }

    return 0;
}

#if LV_USE_GPU_GLES_SW_MIXED
#include <assert.h>
// From the article https://lupyuen.github.io/pinetime-rust-mynewt/articles/wayland
static void put_px(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    assert(x >= 0); assert(x < SDL_HOR_RES);
    assert(y >= 0); assert(y < SDL_VER_RES);
    int i = (y * SDL_HOR_RES * BYTES_PER_PIXEL) + (x * BYTES_PER_PIXEL);
    monitor.texture_pixels[i++] = r;  //  Red
    monitor.texture_pixels[i++] = g;  //  Green
    monitor.texture_pixels[i++] = b;  //  Blue
}
#endif /* LV_USE_GPU_GLES_SW_MIXED */

static GLuint shader_create(GLenum type, const char *src)
{
    GLint success = 0;

    GLuint shader = glCreateShader(type);

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if(!success)
    {
        GLint info_log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_len);

        char *info_log = malloc(info_log_len+1);
        info_log[info_log_len] = '\0';

        glGetShaderInfoLog(shader, info_log_len, NULL, info_log);
        fprintf(stderr, "Failed to compile shader : %s", info_log);
        free(info_log);
    }

    return shader;
}


static GLuint gl_shader_program_create(const char *vertex_src,
                                const char *fragment_src)

{
    GLuint vertex = shader_create(GL_VERTEX_SHADER, vertex_src);
    GLuint fragment = shader_create(GL_FRAGMENT_SHADER, fragment_src);
    GLuint program = glCreateProgram();

    glAttachShader(program, vertex);
    glAttachShader(program, fragment);

    glLinkProgram(program);

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    return program;
}



static GLuint gl_texture_create(int width, int height, GLubyte *pixels)
{
    GLuint tex_id;
    glGenTextures ( 1, &tex_id );
    glBindTexture ( GL_TEXTURE_2D, tex_id );

    glTexImage2D (
        GL_TEXTURE_2D,
        0,  //  Level
        GL_RGB,
        width,  //  Width
        height,  //  Height
        0,  //  Format
        GL_RGB,
        GL_UNSIGNED_BYTE,
        pixels
    );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    return tex_id;
}



#endif /*USE_SDL_GLES*/
