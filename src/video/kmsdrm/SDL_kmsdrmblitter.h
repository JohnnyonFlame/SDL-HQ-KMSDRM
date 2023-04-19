#include "../../SDL_internal.h"

#ifndef _SDL_kmsdrmblitter_h
#define _SDL_kmsdrmblitter_h

#if SDL_VIDEO_OPENGL_EGL

#include "../SDL_sysvideo.h"
#include "../SDL_egl_c.h"

#include "SDL_egl.h"
#include "SDL_opengl.h"

// Tracks teardown resources
typedef struct KMSDRM_Blitter_Plane {
    EGLSyncKHR fence;
    EGLImageKHR image;
    GLuint texture;
    struct gbm_bo *bo;
} KMSDRM_Blitter_Plane;

typedef struct KMSDRM_Blitter {
    /* OpenGL Surface and Context */
    _THIS;
    void *gles2_obj, *egl_obj;
    EGLSurface *egl_surface;
    EGLDisplay *egl_display;
    SDL_GLContext *gl_context;
    SDL_Window *window;
    EGLConfig config;
    GLuint frag, vert, prog, vbo, vao;
    GLint loc_aVertCoord, loc_aTexCoord, loc_uFBOtex, loc_uProj, loc_uTexSize, loc_uScale;
    GLsizei viewport_width, viewport_height;
    GLint plane_width, plane_height, plane_pitch;
    float mat_projection[4][4];
    float vert_buffer_data[4][4];

    // Triple buffering thread
    SDL_mutex *mutex;
    SDL_cond *cond;
    SDL_Thread *thread;
    int thread_stop;
    int rotation;
    int next;

    struct gbm_surface *gs;
    struct gbm_bo *bo, *next_bo;
    KMSDRM_Blitter_Plane planes[2];

    void *user_data;

    #define SDL_PROC(ret,func,params) ret (APIENTRY *func) params;
    #include "SDL_kmsdrmblitter_egl_funcs.h"
    #include "SDL_kmsdrmblitter_gles_funcs.h"
    #undef SDL_PROC
} KMSDRM_Blitter;

extern int KMSDRM_Post_gbm_bo(_THIS, SDL_WindowData *windata, SDL_DisplayData *dispdata, SDL_VideoData *viddata, struct gbm_bo *bo, struct gbm_bo *next_bo);
extern int KMSDRM_InitBlitter(_THIS, KMSDRM_Blitter *blitter, NativeWindowType nw, int rotation);
extern int KMSDRM_BlitterThread(void *data);
extern void KMSDRM_BlitterInit(KMSDRM_Blitter *blitter);
extern void KMSDRM_BlitterQuit(KMSDRM_Blitter *blitter);

#endif /* SDL_VIDEO_OPENGL_EGL */

#endif /* _SDL_kmsdrmblitter_h */
