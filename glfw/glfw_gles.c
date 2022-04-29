/**
 * @file glfw_gles.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "glfw_gles.h"

#if USE_GLFW_GLES
#if LV_USE_GPU_GLES == 0
# error "LV_USE_GPU_GLES must be enabled"
#endif

#include LV_GPU_GLES_EPOXY_INCLUDE_PATH
#include GLFW_INCLUDE_PATH
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3native.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#define _DEFAULT_SOURCE
#include <unistd.h>
#include <assert.h>
#include <linux/input.h>


/*********************
 *      DEFINES
 *********************/
#if LV_USE_GPU_GLES_SW_MIXED
    #define BYTES_PER_PIXEL 3
#endif

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    GLFWwindow *window;
    
    GLuint program;
    GLint position_location;
    GLint uv_location;

    GLuint texture;

#if LV_USE_GPU_GLES_SW_MIXED
    GLubyte *texture_pixels;
#else
    GLuint framebuffer;
#endif /* LV_USE_GPU_GLES_SW_MIXED */
    volatile bool glfw_refr_qry;
    pthread_t tick_thread;
}monitor_t;

typedef struct {
    int16_t x;
    int16_t y;
    bool left_button_down;
} mouse_t;

typedef struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct wl_shell *shell;
    struct wl_shell_surface *shell_surface;
    struct wl_region *region;
    struct wl_egl_window *egl_window;
    struct wl_callback *callback;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_touch *touch;

    int mouse_x;
    int mouse_y;
    bool mouse_pressed;

    int touch_x;
    int touch_y;
    bool touched;


    struct timeval t1,t2;
    double dt;
}wayland_platform_t;

static wayland_platform_t wp;



/**********************
 *  STATIC PROTOTYPES
 **********************/
static void window_create(monitor_t * m);
static void window_update(monitor_t * m);
static void monitor_glfw_gles_refr(lv_timer_t *t);
static void monitor_glfw_gles_clean_up(void);
static void *tick_thread(void *ptr);
static void handle_events();
static GLuint shader_create(GLenum type, const char *src);
static GLuint gl_shader_program_create(const char *vertex_src, const char *fragment_src);
#if LV_USE_GPU_GLES_SW_MIXED
static GLuint gl_texture_create(int width, int height, GLubyte *pixels);
// From the article https://lupyuen.github.io/pinetime-rust-mynewt/articles/wayland
static void put_px(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
#endif /* LV_USE_GPU_GLES_SW_MIXED */
static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
/**********************
 *  STATIC VARIABLES
 **********************/
static monitor_t monitor;
static mouse_t mouse;
static volatile bool quit = false;

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

void glfw_gles_init(void)
{
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window_create(&monitor);

    pthread_create(&monitor.tick_thread, NULL, tick_thread, NULL);
}

void glfw_gles_disp_draw_buf_init(lv_disp_draw_buf_t *draw_buf)
{
#if LV_USE_GPU_GLES_SW_MIXED
    static lv_color_t buf1_1[GLFW_HOR_RES * GLFW_VER_RES];
    lv_disp_draw_buf_init(draw_buf, buf1_1, NULL, GLFW_HOR_RES * GLFW_VER_RES);
#else
    lv_disp_draw_buf_init(draw_buf, NULL, NULL, GLFW_HOR_RES * GLFW_VER_RES);
#endif /* LV_USE_GPU_GLES_SW_MIXED */
}

void glfw_gles_disp_drv_init(lv_disp_drv_t *driver, lv_disp_draw_buf_t *draw_buf)
{
    lv_disp_drv_init(driver);
    driver->draw_buf = draw_buf;
    driver->flush_cb = glfw_gles_display_flush;
    driver->hor_res = GLFW_HOR_RES;
    driver->ver_res = GLFW_VER_RES;
    driver->direct_mode = 1;
    driver->full_refresh = 1;
#if !LV_USE_GPU_GLES_SW_MIXED
    driver->user_data = &monitor.framebuffer;
#endif /* !LV_USE_GPU_GLES_SW_MIXED */
}

void glfw_gles_display_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
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
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLFW_HOR_RES, GLFW_VER_RES, GL_RGB, GL_UNSIGNED_BYTE, monitor.texture_pixels);
    monitor.glfw_refr_qry = true;
    monitor_glfw_gles_refr(NULL);
    lv_disp_flush_ready(disp_drv);
#else
    lv_coord_t hres = disp_drv->hor_res;
    lv_coord_t vres = disp_drv->ver_res;


    /*Return if the area is out the screen*/
    if(area->x2 < 0 || area->y2 < 0 || area->x1 > hres - 1 || area->y1 > vres - 1) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    monitor.glfw_refr_qry = true;

    /* TYPICALLY YOU DO NOT NEED THIS
     * If it was the last part to refresh update the texture of the window.*/
    if(lv_disp_flush_is_last(disp_drv)) {
        monitor_glfw_gles_refr(NULL);
    }

    /*IMPORTANT! It must be called to tell the system the flush is ready*/
    lv_disp_flush_ready(disp_drv);
#endif /* LV_USE_GPU_GLES_SW_MIXED */
}


void glfw_gles_mouse_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    (void) indev_drv;

    data->point.x = wp.mouse_x;
    data->point.y = wp.mouse_y;

    if(wp.mouse_pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void glfw_gles_touch_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    (void) indev_drv;

    data->point.x = wp.touch_x;
    data->point.y = wp.touch_y;

    if(wp.touched) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

}


/**********************
 *   STATIC FUNCTIONS
 **********************/

static void mouse_read_cb(lv_indev_drv_t * drv, lv_indev_data_t*data) {
    if (wp.mouse_pressed) {
        data->point.x = wp.mouse_x;
        data->point.y = wp.mouse_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

}


static void
handle_ping(void *data, struct wl_shell_surface *shell_surface,
            uint32_t serial) {
    wl_shell_surface_pong(shell_surface, serial);
    //  fprintf(stderr, "Pinged and ponged\n");
}

static void
handle_configure(void *data, struct wl_shell_surface *shell_surface,
                 uint32_t edges, int32_t width, int32_t height) {
}

static void
handle_popup_done(void *data, struct wl_shell_surface *shell_surface) {
}

static const struct wl_shell_surface_listener shell_surface_listener = {
    handle_ping,
    handle_configure,
    handle_popup_done
};



static void
wp_pointer_handle_enter(void *data, struct wl_pointer *pointer,
                        uint32_t serial, struct wl_surface *surface,
                        wl_fixed_t sx, wl_fixed_t sy) {
    struct wl_buffer *buffer;
    struct wl_cursor_image *image;

    // fprintf(stderr, "Pointer entered surface %p at %d %d\n", surface, sx, sy);

}

static void
wp_pointer_handle_leave(void *data, struct wl_pointer *pointer,
                        uint32_t serial, struct wl_surface *surface) {
    //  fprintf(stderr, "Pointer left surface %p\n", surface);
}

static void
wp_pointer_handle_motion(void *data, struct wl_pointer *pointer,
                         uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    int x = wl_fixed_to_int(sx);
    int y = wl_fixed_to_int(sy);
    //printf("Pointer moved at %d %d\n", x, y);
    wp.mouse_x = x;
    wp.mouse_y = y;
}

static void
wp_pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                         uint32_t serial, uint32_t time, uint32_t button,
                         uint32_t state) {

    if ( button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED ) {
        wp.mouse_pressed = true;
    } else if ( button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_RELEASED ) {
        wp.mouse_pressed = false;
    }


}

static void
wp_pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                       uint32_t time, uint32_t axis, wl_fixed_t value) {
    //printf("Pointer handle axis\n");
}


static const struct wl_pointer_listener wp_pointer_listener = {
    wp_pointer_handle_enter,
    wp_pointer_handle_leave,
    wp_pointer_handle_motion,
    wp_pointer_handle_button,
    wp_pointer_handle_axis,
};


void wp_touch_handle_down(void *data, struct wl_touch *wl_touch,
                          uint32_t serial, uint32_t time,
                          struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    int int_x = wl_fixed_to_int(x);
    int int_y = wl_fixed_to_int(y);
    wp.touch_x = int_x;
    wp.touch_y = int_y;
    wp.touched = true;
}

void wp_touch_handle_up(void *data,
           struct wl_touch *wl_touch,
           uint32_t serial,
           uint32_t time,
           int32_t id) {

    wp.touched = false;

}


void wp_touch_handle_motion(void *data,
               struct wl_touch *wl_touch,
               uint32_t time,
               int32_t id,
               wl_fixed_t x,
               wl_fixed_t y) {

    int int_x = wl_fixed_to_int(x);
    int int_y = wl_fixed_to_int(y);
    wp.touch_x = int_x;
    wp.touch_y = int_y;
    wp.touched = true;


}

void wp_touch_handle_frame(void *data,
              struct wl_touch *wl_touch) {

}

void wp_touch_handle_shape(void *data,
              struct wl_touch *wl_touch,
              int32_t id,
              wl_fixed_t major,
              wl_fixed_t minor) {

}

void wp_touch_handle_orientation(void *data,
                    struct wl_touch *wl_touch,
                    int32_t id,
                    wl_fixed_t orientation) {

}


static const struct wl_touch_listener wp_touch_listener = {
    wp_touch_handle_down,
    wp_touch_handle_up,
    wp_touch_handle_motion,
    wp_touch_handle_frame,
    wp_touch_handle_shape,
    wp_touch_handle_orientation,
};

static void
wp_seat_handle_capabilities(void *data, struct wl_seat *seat,
                            enum wl_seat_capability caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wp.pointer) {
        wp.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wp.pointer, &wp_pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wp.pointer) {
        wl_pointer_destroy(wp.pointer);
        wp.pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !wp.touch) {
        wp.touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(wp.touch, &wp_touch_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && wp.touch) {
        wl_touch_destroy(wp.touch);
        wp.touch = NULL;
    }

}

static const struct wl_seat_listener wp_seat_listener = {
    wp_seat_handle_capabilities,
};




static void wp_global_handler(void *data, struct wl_registry *registry,
                              uint32_t name, const char *iface, uint32_t ver) {
    if (strcmp(iface, "wl_compositor") == 0) {
        wp.compositor = wl_registry_bind(registry,
                                         name,
                                         &wl_compositor_interface,
                                         1);
    } else if (strcmp(iface, "wl_shell") == 0) {
        wp.shell = wl_registry_bind(registry,
                                    name,
                                    &wl_shell_interface,
                                    1);
    }
    else if (strcmp(iface, "wl_seat") == 0) {
        wp.seat = wl_registry_bind(registry,
                                   name,
                                   &wl_seat_interface,
                                   1);

        wl_seat_add_listener(wp.seat, &wp_seat_listener, NULL);
    }

}

static void wp_global_remove_handler(void *data, struct wl_registry *registry,
                                     uint32_t name) {
    // Empty
}


static const struct wl_registry_listener wp_registry_listener = {
    .global = wp_global_handler,
    .global_remove = wp_global_remove_handler
};



static void window_create(monitor_t * m)
{
    m->window = glfwCreateWindow(GLFW_HOR_RES,
                                 GLFW_VER_RES,
                                 "lvgl-opengl",
                                 NULL,
                                 NULL);

    glfwMakeContextCurrent(m->window);
    glfwSwapInterval(0);
    glfwSetFramebufferSizeCallback(m->window, framebuffer_size_callback);

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
    m->texture_pixels = malloc(GLFW_HOR_RES * GLFW_VER_RES * BYTES_PER_PIXEL * sizeof(GLubyte));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, GLFW_HOR_RES, GLFW_VER_RES, 0, GL_RGB, GL_UNSIGNED_BYTE, m->texture_pixels);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLFW_HOR_RES, GLFW_VER_RES, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &m->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m->texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif /* LV_USE_GPU_GLES_SW_MIXED */

    glBindTexture(GL_TEXTURE_2D, 0);
    m->glfw_refr_qry = true;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    wp.display = glfwGetWaylandDisplay();
    struct wl_registry *registry = wl_display_get_registry(wp.display);
    assert(registry);
    wl_registry_add_listener(registry, &wp_registry_listener, NULL);

    wl_display_dispatch(wp.display);
    wl_display_roundtrip(wp.display);



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


    glfwSwapBuffers(m->window);
}


static void monitor_glfw_gles_refr(lv_timer_t *t)
{    (void)t;
    /*Refresh handling*/
    if(monitor.glfw_refr_qry != false) {
        monitor.glfw_refr_qry = false;
        window_update(&monitor);
    }
}


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

static void *tick_thread(void *ptr)
{
    (void)ptr;

    while(!quit) {
        handle_events();
        lv_tick_inc(5);
        usleep(5 * 1000);
    }
}

static void handle_events()
{
    glfwPollEvents();

    if(glfwWindowShouldClose(monitor.window)) {
        quit = true;
        monitor_glfw_gles_clean_up();
        /* TODO: this is temp solution for the exit */
        exit(EXIT_SUCCESS);
    }
}

static void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    (void) window;
    glViewport(0, 0, width, height);
}

static void monitor_glfw_gles_clean_up(void)
{
    glfwDestroyWindow(monitor.window);
    glfwTerminate();
}



#endif /*USE_GLFW_GLES*/