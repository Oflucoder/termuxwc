#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "protocols/xdg-shell-protocol.h"

static volatile bool running = true;

static void sigint_handler(int sig) {
    (void)sig;
    running = false;
}

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_egl_window *egl_window;

    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;

    GLuint program;
    GLint u_mvp;
    GLint a_pos;
    GLint a_color;

    int width;
    int height;
    bool configured;
    float angle_x;
    float angle_y;

    struct timespec last_fps_time;
    int frame_count;
};

// Shaders
static const char *vert_src =
    "attribute vec3 a_pos;\n"
    "attribute vec3 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *frag_src =
    "precision mediump float;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(v_color, 1.0);\n"
    "}\n";

// Cube vertices: position (x,y,z) + color (r,g,b)
static const GLfloat cube_vertices[] = {
    // Front face (Red)
    -0.5f, -0.5f,  0.5f,   1.0f, 0.2f, 0.2f,
     0.5f, -0.5f,  0.5f,   1.0f, 0.2f, 0.2f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.2f, 0.2f,
    -0.5f, -0.5f,  0.5f,   1.0f, 0.2f, 0.2f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.2f, 0.2f,
    -0.5f,  0.5f,  0.5f,   1.0f, 0.2f, 0.2f,

    // Back face (Green)
    -0.5f, -0.5f, -0.5f,   0.2f, 1.0f, 0.2f,
     0.5f,  0.5f, -0.5f,   0.2f, 1.0f, 0.2f,
     0.5f, -0.5f, -0.5f,   0.2f, 1.0f, 0.2f,
    -0.5f, -0.5f, -0.5f,   0.2f, 1.0f, 0.2f,
    -0.5f,  0.5f, -0.5f,   0.2f, 1.0f, 0.2f,
     0.5f,  0.5f, -0.5f,   0.2f, 1.0f, 0.2f,

    // Top face (Blue)
    -0.5f,  0.5f, -0.5f,   0.2f, 0.4f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.2f, 0.4f, 1.0f,
     0.5f,  0.5f,  0.5f,   0.2f, 0.4f, 1.0f,
    -0.5f,  0.5f, -0.5f,   0.2f, 0.4f, 1.0f,
     0.5f,  0.5f,  0.5f,   0.2f, 0.4f, 1.0f,
     0.5f,  0.5f, -0.5f,   0.2f, 0.4f, 1.0f,

    // Bottom face (Yellow)
    -0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.2f,
     0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.2f,
    -0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.2f,
    -0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.2f,
     0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.2f,
     0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.2f,

    // Right face (Magenta)
     0.5f, -0.5f, -0.5f,   1.0f, 0.2f, 1.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.2f, 1.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 0.2f, 1.0f,
     0.5f, -0.5f, -0.5f,   1.0f, 0.2f, 1.0f,
     0.5f,  0.5f, -0.5f,   1.0f, 0.2f, 1.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.2f, 1.0f,

    // Left face (Cyan)
    -0.5f, -0.5f, -0.5f,   0.2f, 1.0f, 1.0f,
    -0.5f, -0.5f,  0.5f,   0.2f, 1.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.2f, 1.0f, 1.0f,
    -0.5f, -0.5f, -0.5f,   0.2f, 1.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.2f, 1.0f, 1.0f,
     0.5f, -0.5f, -0.5f,   0.2f, 1.0f, 1.0f,
};

static void multiply_matrix(float out[16], const float a[16], const float b[16]) {
    float res[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            res[col * 4 + row] = sum;
        }
    }
    memcpy(out, res, sizeof(res));
}

static void identity_matrix(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void perspective_matrix(float m[16], float fov_rad, float aspect, float near_z, float far_z) {
    memset(m, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fov_rad / 2.0f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far_z + near_z) / (near_z - far_z);
    m[11] = -1.0f;
    m[14] = (2.0f * far_z * near_z) / (near_z - far_z);
}

static void rotate_x_matrix(float m[16], float rad) {
    identity_matrix(m);
    float c = cosf(rad);
    float s = sinf(rad);
    m[5] = c;
    m[6] = s;
    m[9] = -s;
    m[10] = c;
}

static void rotate_y_matrix(float m[16], float rad) {
    identity_matrix(m);
    float c = cosf(rad);
    float s = sinf(rad);
    m[0] = c;
    m[2] = -s;
    m[8] = s;
    m[10] = c;
}

static void translate_matrix(float m[16], float x, float y, float z) {
    identity_matrix(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static void render_frame(struct app *app) {
    if (!running || !app->configured) return;

    glViewport(0, 0, app->width, app->height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float proj[16], trans[16], rot_x[16], rot_y[16], rot[16], model[16], mvp[16];
    float aspect = (float)app->width / (float)(app->height > 0 ? app->height : 1);
    perspective_matrix(proj, 45.0f * (M_PI / 180.0f), aspect, 0.1f, 100.0f);

    translate_matrix(trans, 0.0f, 0.0f, -3.0f);
    rotate_x_matrix(rot_x, app->angle_x);
    rotate_y_matrix(rot_y, app->angle_y);

    multiply_matrix(rot, rot_x, rot_y);
    multiply_matrix(model, trans, rot);
    multiply_matrix(mvp, proj, model);

    glUniformMatrix4fv(app->u_mvp, 1, GL_FALSE, mvp);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    eglSwapBuffers(app->egl_display, app->egl_surface);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double swap_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    app->angle_x += 0.02f;
    app->angle_y += 0.03f;
    app->frame_count++;

    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    double elapsed = (current_time.tv_sec - app->last_fps_time.tv_sec) +
                     (current_time.tv_nsec - app->last_fps_time.tv_nsec) / 1e9;
    if (elapsed >= 1.0) {
        double fps = app->frame_count / elapsed;
        printf("\r>> [termuxwc-cube] Running on GPU | FPS: %.1f (Swap: %.1f ms) ", fps, swap_ms);
        fflush(stdout);
        app->frame_count = 0;
        app->last_fps_time = current_time;
    }
}

// XDG Shell Listeners
static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct app *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    app->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                                   int32_t width, int32_t height, struct wl_array *states) {
    struct app *app = data;
    (void)xdg_toplevel;
    (void)states;
    if (width > 0 && height > 0) {
        app->width = width;
        app->height = height;
        if (app->egl_window) {
            wl_egl_window_resize(app->egl_window, width, height, 0, 0);
        }
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    (void)data;
    (void)xdg_toplevel;
    running = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

// Wayland Registry Listener
static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    struct app *app = data;
    (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
        fprintf(stderr, "Shader compile error: %s\n", buf);
        exit(1);
    }
    return s;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    struct app app = {
        .width = 640,
        .height = 480,
        .angle_x = 0.0f,
        .angle_y = 0.0f,
        .configured = false,
        .frame_count = 0,
    };
    clock_gettime(CLOCK_MONOTONIC, &app.last_fps_time);

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "Error: Could not connect to Wayland display\n");
        return 1;
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.wm_base) {
        fprintf(stderr, "Error: Compositor does not support needed Wayland protocols\n");
        return 1;
    }

    // EGL Setup
    setenv("EGL_LOG_LEVEL", "fatal", 0);
    app.egl_display = eglGetDisplay((EGLNativeDisplayType)app.display);
    if (app.egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Error: eglGetDisplay failed\n");
        return 1;
    }

    EGLint major, minor;
    if (!eglInitialize(app.egl_display, &major, &minor)) {
        fprintf(stderr, "Error: eglInitialize failed\n");
        return 1;
    }

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(app.egl_display, config_attribs, &app.egl_config, 1, &num_configs) || num_configs < 1) {
        fprintf(stderr, "Error: eglChooseConfig failed\n");
        return 1;
    }

    EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    app.egl_context = eglCreateContext(app.egl_display, app.egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (app.egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Error: eglCreateContext failed\n");
        return 1;
    }

    // Wayland Surface & Window setup
    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);

    app.xdg_toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.xdg_toplevel, &xdg_toplevel_listener, &app);
    xdg_toplevel_set_title(app.xdg_toplevel, "termuxwc GPU Accelerated Cube");
    xdg_toplevel_set_app_id(app.xdg_toplevel, "termuxwc-cube");

    app.egl_window = wl_egl_window_create(app.surface, app.width, app.height);
    app.egl_surface = eglCreateWindowSurface(app.egl_display, app.egl_config,
                                            (EGLNativeWindowType)app.egl_window, NULL);
    if (app.egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Error: eglCreateWindowSurface failed\n");
        return 1;
    }

    if (!eglMakeCurrent(app.egl_display, app.egl_surface, app.egl_surface, app.egl_context)) {
        fprintf(stderr, "Error: eglMakeCurrent failed\n");
        return 1;
    }

    // Print active GPU hardware
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);

    printf("====================================================\n");
    printf("   termuxwc Hardware GPU Accelerated Wayland Cube    \n");
    printf("====================================================\n");
    printf(">> Vendor   : %s\n", vendor ? vendor : "Unknown");
    printf(">> Renderer : %s\n", renderer ? renderer : "Unknown");
    printf(">> Version  : %s\n", version ? version : "Unknown");
    printf(">> Surface  : Native Wayland EGL (%dx%d)\n", app.width, app.height);
    printf("====================================================\n");

    // Shaders setup
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    app.program = glCreateProgram();
    glAttachShader(app.program, vs);
    glAttachShader(app.program, fs);
    glLinkProgram(app.program);
    glUseProgram(app.program);

    app.u_mvp = glGetUniformLocation(app.program, "u_mvp");
    app.a_pos = glGetAttribLocation(app.program, "a_pos");
    app.a_color = glGetAttribLocation(app.program, "a_color");

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube_vertices), cube_vertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(app.a_pos);
    glVertexAttribPointer(app.a_pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (void *)0);

    glEnableVertexAttribArray(app.a_color);
    glVertexAttribPointer(app.a_color, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (void *)(3 * sizeof(GLfloat)));

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glClearColor(0.12f, 0.14f, 0.20f, 1.0f);

    wl_surface_commit(app.surface);

    // Initial configure roundtrip
    while (running && !app.configured) {
        if (wl_display_dispatch(app.display) == -1) break;
    }

    struct pollfd pfd = {
        .fd = wl_display_get_fd(app.display),
        .events = POLLIN,
    };

    while (running) {
        while (wl_display_prepare_read(app.display) != 0) {
            wl_display_dispatch_pending(app.display);
        }
        wl_display_flush(app.display);

        int ret = poll(&pfd, 1, 8); // 8ms poll (~120 FPS target)
        if (ret > 0) {
            wl_display_read_events(app.display);
        } else {
            wl_display_cancel_read(app.display);
        }
        wl_display_dispatch_pending(app.display);

        render_frame(&app);
    }

    printf("\nExiting termuxwc-cube cleanly.\n");

    glDeleteBuffers(1, &vbo);
    glDeleteProgram(app.program);
    eglDestroySurface(app.egl_display, app.egl_surface);
    wl_egl_window_destroy(app.egl_window);
    xdg_toplevel_destroy(app.xdg_toplevel);
    xdg_surface_destroy(app.xdg_surface);
    wl_surface_destroy(app.surface);
    eglDestroyContext(app.egl_display, app.egl_context);
    eglTerminate(app.egl_display);
    xdg_wm_base_destroy(app.wm_base);
    wl_compositor_destroy(app.compositor);
    wl_registry_destroy(app.registry);
    wl_display_disconnect(app.display);

    return 0;
}
