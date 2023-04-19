#include "../../SDL_internal.h"

#if SDL_VIDEO_OPENGL_EGL

#include "SDL.h"
#include "SDL_egl.h"
#include "SDL_opengl.h"

#include "SDL_kmsdrmdyn.h"
#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmblitter.h"

/* used to simplify code */
typedef struct mat4 {
    GLfloat v[16];
} mat4;

static GLchar* blit_vert_fmt =
"#version 100\n"
"varying vec2 vTexCoord;\n"
"attribute vec2 aVertCoord;\n"
"attribute vec2 aTexCoord;\n"
"uniform mat4 uProj;\n"
"uniform vec2 uTexSize;\n"
"void main() {\n"
"   %s\n"
"   %s\n"
"   gl_Position = uProj * vec4(aVertCoord, 0.0, 1.0);\n"
"}";

static GLchar* blit_frag_standard =
"#version 100\n"
"precision mediump float;\n"
"varying vec2 vTexCoord;\n"
"uniform sampler2D uFBOTex;\n"
"uniform vec2 uTexSize;\n"
"uniform vec2 uScale;\n"
"void main() {\n"
"   vec2 texel_floored = floor(vTexCoord);\n"
"   gl_FragColor = texture2D(uFBOTex, vTexCoord);\n"
"}\n";

// Ported from TheMaister's sharp-bilinear-simple.slang
static GLchar* blit_frag_bilinear_simple =
"#version 100\n"
"precision mediump float;\n"
"varying vec2 vTexCoord;\n"
"uniform sampler2D uFBOTex;\n"
"uniform vec2 uTexSize;\n"
"uniform vec2 uScale;\n"
"void main() {\n"
"   vec2 texel_floored = floor(vTexCoord);\n"
"   vec2 s = fract(vTexCoord);\n"
"   vec2 region_range = 0.5 - 0.5 / uScale;\n"
"   vec2 center_dist = s - 0.5;\n"
"   vec2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * uScale + 0.5;\n"
"   vec2 mod_texel = texel_floored + f;\n"
"   gl_FragColor = texture2D(uFBOTex, mod_texel / uTexSize);\n"
"}\n";

// Ported from Iquilez
static GLchar* blit_frag_quilez =
"#version 100\n"
"precision highp float;\n"
"varying vec2 vTexCoord;\n"
"uniform sampler2D uFBOTex;\n"
"uniform vec2 uTexSize;\n"
"uniform vec2 uScale;\n"
"void main() {\n"
"   vec2 p = vTexCoord + 0.5;\n"
"   vec2 i = floor(p);\n"
"   vec2 f = p - i;\n"
"   f = f*f*f*(f*(f*6.0-15.0)+10.0);\n"
"   p = i + f;\n"
"   p = (p - 0.5)/uTexSize;\n"
"   gl_FragColor = texture2D( uFBOTex, p );\n"
"}\n";

SDL_GLContext
KMSDRM_Blitter_CreateContext(_THIS, KMSDRM_Blitter *blitter, EGLSurface egl_surface)
{
    EGLContext egl_context;
    /* max 14 values plus terminator. */
    EGLint attribs[15];
    int attr = 0;

    if (!_this->egl_data) {
        SDL_SetError("EGL not initialized");
        return NULL;
    }

    attribs[attr++] = EGL_CONTEXT_MAJOR_VERSION_KHR;
    attribs[attr++] = 2;
    attribs[attr++] = EGL_CONTEXT_MINOR_VERSION_KHR;
    attribs[attr++] = 0;

    /* SDL flags match EGL flags. */
    if (_this->gl_config.flags != 0) {
        attribs[attr++] = EGL_CONTEXT_FLAGS_KHR;
        attribs[attr++] = _this->gl_config.flags;
    }

    attribs[attr++] = EGL_NONE;
    
    // TODO:: Figure out how to properly handle these
    // as to properly decouple everything from the internal SDL functions.
    blitter->egl_display = _this->egl_data->egl_display;
    blitter->config = _this->egl_data->egl_config;

    blitter->eglBindAPI(_this->egl_data->apitype);
    egl_context = blitter->eglCreateContext(blitter->egl_display,
                                      blitter->config,
                                      EGL_NO_CONTEXT, attribs);

    if (egl_context == EGL_NO_CONTEXT) {
        SDL_EGL_SetError("Could not create EGL context", "eglCreateContext");
        return NULL;
    }

    return (SDL_GLContext) egl_context;
}

static void
get_aspect_correct_coords(int viewport[2], int plane[2], int rotation, GLfloat vert[4][4], GLfloat scale[2])
{
    float aspect_plane, aspect_viewport, ratio_x, ratio_y;
    int shift_x, shift_y, temp;

    // when sideways, invert plane coords
    if (rotation & 1) {
        temp = plane[0];
        plane[0] = plane[1];
        plane[1] = temp;
    }

    // Choose which edge to touch
    aspect_plane = (float)plane[0] / plane[1];
    aspect_viewport = (float)viewport[0] / viewport[1];

    if (aspect_viewport > aspect_plane) {
        // viewport wider than plane
        ratio_x = plane[0] * ((float)viewport[1] / plane[1]);
        ratio_y = viewport[1];
        shift_x = (viewport[0] - ratio_x) / 2.0f;
        shift_y = 0;
    } else {
        // plane wider than viewport
        ratio_x = viewport[0];
        ratio_y = plane[1] * ((float)viewport[0] / plane[0]);
        shift_x = 0;
        shift_y = (viewport[1] - ratio_y) / 2.0f;
    }

    // Instead of normalized UVs, use full texture size.
    vert[0][2] = (int)(0.0f * plane[0]); vert[0][3] = (int)(0.0f * plane[1]);
    vert[1][2] = (int)(0.0f * plane[0]); vert[1][3] = (int)(1.0f * plane[1]);
    vert[2][2] = (int)(1.0f * plane[0]); vert[2][3] = (int)(0.0f * plane[1]);
    vert[3][2] = (int)(1.0f * plane[0]); vert[3][3] = (int)(1.0f * plane[1]);

    // Get aspect corrected sizes within pixel boundaries
    vert[0][0] = (int)(0.0f * ratio_x) + shift_x; vert[0][1] = (int)(0.0f * ratio_y) + shift_y;
    vert[1][0] = (int)(0.0f * ratio_x) + shift_x; vert[1][1] = (int)(1.0f * ratio_y) + shift_y;
    vert[2][0] = (int)(1.0f * ratio_x) + shift_x; vert[2][1] = (int)(0.0f * ratio_y) + shift_y;
    vert[3][0] = (int)(1.0f * ratio_x) + shift_x; vert[3][1] = (int)(1.0f * ratio_y) + shift_y;

    // Get scale, for filtering.
    scale[0] = ratio_x / plane[0];
    scale[1] = ratio_y / plane[1];
}

static
void mat_ortho(float left, float right, float bottom, float top, float Result[4][4])
{
    *(mat4*)Result = (mat4){{[0 ... 15] = 0}};
    Result[0][0] = 2.0f / (right - left);
    Result[1][1] = 2.0f / (top - bottom);
    Result[2][2] = -1.0f;
    Result[3][0] = - (right + left) / (right - left);
    Result[3][1] = - (top + bottom) / (top - bottom);
    Result[3][3] = 1.0f;
}

#define fourcc_code(a, b, c, d) \
      ((uint32_t)(a) \
    | ((uint32_t)(b) << 8) \
    | ((uint32_t)(c) << 16) \
    | ((uint32_t)(d) << 24))

int
KMSDRM_InitBlitter(_THIS, KMSDRM_Blitter *blitter, NativeWindowType nw, int rotation)
{
    int fail = 0;
    char *use_hq_scaler;
    GLchar msg[2048] = {}, blit_vert[2048] = {};
    const GLchar *sources[2] = { blit_vert, blit_frag_standard };
    float scale[2];

    if ((use_hq_scaler = SDL_getenv("SDL_KMSDRM_HQ_SCALER")) != NULL && *use_hq_scaler != '0') {
        switch (*use_hq_scaler) {
            case '1': sources[1] = blit_frag_bilinear_simple; break;
            case '2': sources[1] = blit_frag_quilez; break;
            default: use_hq_scaler = NULL; break;
        }
    } else {
        use_hq_scaler = NULL;
    }

    blitter->egl_obj = SDL_LoadObject("libEGL.so");
    blitter->gles2_obj = SDL_LoadObject("libGLESv2.so");
    if (!blitter->egl_obj || !blitter->gles2_obj) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed loading one or more dynamic libraries (%p %p).", blitter->gles2_obj, blitter->egl_obj);
        return 0;
    }

    if ((blitter->eglGetProcAddress = SDL_LoadFunction(blitter->egl_obj, "eglGetProcAddress")) == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Could not locate eglGetProcAddress.");
        return 0;
    }

    /* Attempt to initialize necessary functions */
    #define SDL_PROC(ret,func,params) \
        blitter->func = blitter->eglGetProcAddress(#func); \
        if (blitter->func == NULL) \
        { \
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed loading \"%s\".", #func); \
            fail = 1; \
        }
    #include "SDL_kmsdrmblitter_egl_funcs.h"
    #include "SDL_kmsdrmblitter_gles_funcs.h"
    #undef SDL_PROC

    if (fail) {
        return 0;
    }

    blitter->egl_surface = SDL_EGL_CreateSurface(_this, nw);
    if (blitter->egl_surface == EGL_NO_SURFACE) {
        SDL_EGL_SetError("No surface pilfered from backend, failure.", "SDL_EGL_CreateSurface");
        return 0;
    }

    blitter->gl_context = KMSDRM_Blitter_CreateContext(_this, blitter, blitter->egl_surface);
    if (blitter->gl_context == EGL_NO_CONTEXT) {
        SDL_EGL_SetError("Failed to setup blitter EGL Context", "SDL_EGL_CreateContext");
        return 0;
    }
    
    if (!blitter->eglMakeCurrent(blitter->egl_display,
        blitter->egl_surface,
        blitter->egl_surface,
        blitter->gl_context))
    {
        SDL_EGL_SetError("Unable to make blitter EGL context current", "eglMakeCurrent");
        return 0;
    }

    /* Setup vertex shader coord orientation */
    SDL_snprintf(blit_vert, sizeof(blit_vert), blit_vert_fmt,
        /* rotation */
        (rotation == 0) ? "vTexCoord = vec2(aTexCoord.x, -aTexCoord.y);" :
        (rotation == 1) ? "vTexCoord = vec2(aTexCoord.y, aTexCoord.x);" :
        (rotation == 2) ? "vTexCoord = vec2(-aTexCoord.x, aTexCoord.y);" :
        (rotation == 3) ? "vTexCoord = vec2(-aTexCoord.y, -aTexCoord.x);" :
        "#error Orientation out of scope",
        /* scalers */
        (use_hq_scaler) ? "vTexCoord = vTexCoord;"
                        : "vTexCoord = vTexCoord / uTexSize;");

    /* Compile vertex shader */
    blitter->vert = blitter->glCreateShader(GL_VERTEX_SHADER);
    blitter->glShaderSource(blitter->vert, 1, &sources[0], NULL);
    blitter->glCompileShader(blitter->vert);
    blitter->glGetShaderInfoLog(blitter->vert, sizeof(msg), NULL, msg);
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Blitter Vertex Shader Info: %s\n", msg);

    /* Compile the fragment shader */
    blitter->frag = blitter->glCreateShader(GL_FRAGMENT_SHADER);
    blitter->glShaderSource(blitter->frag, 1, &sources[1], NULL);
    blitter->glCompileShader(blitter->frag);
    blitter->glGetShaderInfoLog(blitter->frag, sizeof(msg), NULL, msg);
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Blitter Fragment Shader Info: %s\n", msg);

    blitter->prog = blitter->glCreateProgram();
    blitter->glAttachShader(blitter->prog, blitter->vert);
    blitter->glAttachShader(blitter->prog, blitter->frag);

    blitter->glLinkProgram(blitter->prog);
    blitter->loc_aVertCoord = blitter->glGetAttribLocation(blitter->prog, "aVertCoord");
    blitter->loc_aTexCoord = blitter->glGetAttribLocation(blitter->prog, "aTexCoord");
    blitter->loc_uFBOtex = blitter->glGetUniformLocation(blitter->prog, "uFBOTex");
    blitter->loc_uProj = blitter->glGetUniformLocation(blitter->prog, "uProj");
    blitter->loc_uTexSize = blitter->glGetUniformLocation(blitter->prog, "uTexSize");
    blitter->loc_uScale = blitter->glGetUniformLocation(blitter->prog, "uScale");

    blitter->glGetProgramInfoLog(blitter->prog, sizeof(msg), NULL, msg);
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Blitter Program Info: %s\n", msg);

    /* Setup programs */
    blitter->glUseProgram(blitter->prog);
    blitter->glUniform1i(blitter->loc_uFBOtex, 0);

    /* Prepare projection and aspect corrected bounds */
    mat_ortho(0, blitter->viewport_width, 0, blitter->viewport_height, blitter->mat_projection);
    get_aspect_correct_coords(
        (int [2]){blitter->viewport_width, blitter->viewport_height},
        (int [2]){blitter->plane_width, blitter->plane_height},
        rotation,
        blitter->vert_buffer_data,
        scale
    );

    /* Setup viewport, projection, scale, texture size */
    blitter->glViewport(0, 0, blitter->viewport_width, blitter->viewport_height);
    blitter->glUniformMatrix4fv(blitter->loc_uProj, 1, 0, (GLfloat*)blitter->mat_projection);
    blitter->glUniform2f(blitter->loc_uScale, scale[0], scale[1]);
    blitter->glUniform2f(blitter->loc_uTexSize, blitter->plane_width, blitter->plane_height);

    /* Generate buffers */
    blitter->glGenBuffers(1, &blitter->vbo);
    blitter->glGenVertexArraysOES(1, &blitter->vao);

    /* Populate buffers */
    blitter->glBindVertexArrayOES(blitter->vao);
    blitter->glBindBuffer(GL_ARRAY_BUFFER, blitter->vbo);
    blitter->glEnableVertexAttribArray(blitter->loc_aVertCoord);
    blitter->glEnableVertexAttribArray(blitter->loc_aTexCoord);
    blitter->glVertexAttribPointer(blitter->loc_aVertCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(0 * sizeof(float)));
    blitter->glVertexAttribPointer(blitter->loc_aTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    blitter->glBufferData(GL_ARRAY_BUFFER, sizeof(blitter->vert_buffer_data), blitter->vert_buffer_data, GL_STATIC_DRAW);
    return 1;
}

void
KMSDRM_Blitter_Blit(_THIS, KMSDRM_Blitter *blitter, GLuint texture)
{
    blitter->glBindVertexArrayOES(blitter->vao);
    blitter->glBindTexture(GL_TEXTURE_2D, texture);
    blitter->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void
KMSDRM_Blitter_GetTexture(_THIS, KMSDRM_Blitter *blitter, KMSDRM_Blitter_Plane *plane)
{
    int fd = KMSDRM_gbm_bo_get_fd(plane->bo);
    EGLint attribute_list[] = {
        EGL_WIDTH, KMSDRM_gbm_bo_get_width(plane->bo),
        EGL_HEIGHT, KMSDRM_gbm_bo_get_height(plane->bo),
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, KMSDRM_gbm_bo_get_stride(plane->bo),
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_NONE
    };

    if (fd < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to get bo handle! (%d)", fd);
        return;
    }

    plane->image = blitter->eglCreateImageKHR(blitter->egl_display,
        EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        (EGLClientBuffer)NULL,
        attribute_list);
    if (plane->image == EGL_NO_IMAGE_KHR) {
        SDL_EGL_SetError("Failed to create Blitter EGL Image", "eglCreateImageKHR");
        return;
    }

    blitter->glGenTextures(1, &plane->texture);
    blitter->glActiveTexture(GL_TEXTURE0);
    blitter->glBindTexture(GL_TEXTURE_2D, plane->texture);
    blitter->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    blitter->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    blitter->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    blitter->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    blitter->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, plane->image);
    close(fd);
}

void
KMSDRM_Blitter_FreeTexture(_THIS, KMSDRM_Blitter *blitter, KMSDRM_Blitter_Plane *plane)
{
    if (!plane->bo)
        return;

    blitter->glDeleteTextures(1, &plane->texture);
    blitter->eglDestroyImageKHR(blitter->egl_display, plane->image);
    /*
     * It might be tempting to call release_buffer here, but remember that
     * this resource does not belong to the blitter, and is managed by SwapBuffers.
     * //KMSDRM_gbm_surface_release_buffer(blitter->gs, plane->bo);
     */

    plane->bo = NULL;
    plane->image = EGL_NO_IMAGE_KHR;
    plane->texture = -1;
}

int KMSDRM_BlitterThread(void *data)
{
    int i;
    int prevSwapInterval = -1;
    KMSDRM_Blitter_Plane *current;
    KMSDRM_Blitter *blitter = (KMSDRM_Blitter*)data;
    SDL_Window *window = blitter->window;
    _THIS = blitter->_this;
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);
    SDL_WindowData *windata = (SDL_WindowData *)window->driverdata;
    SDL_VideoDisplay *display = SDL_GetDisplayForWindow(window);
    SDL_DisplayData *dispdata = (SDL_DisplayData *)display->driverdata;

    /* Initialize gbm surface */
    blitter->gs = KMSDRM_gbm_surface_create(viddata->gbm_dev,
                      blitter->viewport_width, blitter->viewport_height,
                      GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING); 
    if (!blitter->gs) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to initialize blitter gbm surface");
        SDL_Quit();
    }

    /* Initialize blitter */
    if (!KMSDRM_InitBlitter(_this, blitter, (NativeWindowType)blitter->gs, blitter->rotation))
    {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to initialize blitter thread");
        SDL_Quit();
    }

    /* Signal triplebuf available */
    SDL_LockMutex(blitter->mutex);
    SDL_CondSignal(blitter->cond);

    for (;;) {
        SDL_CondWait(blitter->cond, blitter->mutex);        

        if (blitter->thread_stop)
            break;

        if (prevSwapInterval != _this->egl_data->egl_swapinterval) {
            blitter->eglSwapInterval(blitter->egl_display, _this->egl_data->egl_swapinterval);
            prevSwapInterval = _this->egl_data->egl_swapinterval;
        }

        /* Release previous texture and acquire current */
        KMSDRM_Blitter_FreeTexture(_this, blitter, &blitter->planes[!blitter->next]);
        current = &blitter->planes[blitter->next];
        blitter->next ^= 1;

        /* wait for fence and flip display */
        if (blitter->eglClientWaitSyncKHR(
            blitter->egl_display,
            current->fence, 
            EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 
            EGL_FOREVER_NV))
        {
            /* Discarding previous data... */
            blitter->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            blitter->glClearColor(0.0, 1.0, 0.0, 1.0);

            /* Perform blitting */
            KMSDRM_Blitter_GetTexture(_this, blitter, current);
            KMSDRM_Blitter_Blit(_this, blitter, current->texture);

            /* Wait for confirmation that the next front buffer has been flipped, at which
            point the previous front buffer can be released */
            if (!KMSDRM_WaitPageflip(_this, windata)) {
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Wait for previous pageflip failed");
                return 0;
            }

            /* Release the previous front buffer */
            if (blitter->bo) {
                KMSDRM_gbm_surface_release_buffer(blitter->gs, blitter->bo);
                blitter->bo = NULL;
            }

            blitter->bo = blitter->next_bo;

            /* Mark a buffer to becume the next front buffer.
            This won't happen until pagelip completes. */
            if (!(blitter->eglSwapBuffers(blitter->egl_display, blitter->egl_surface))) {
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "eglSwapBuffers failed");
                return 0;
            }

            /* From the GBM surface, get the next BO to become the next front buffer,
            and lock it so it can't be allocated as a back buffer (to prevent EGL
            from drawing into it!) */
            blitter->next_bo = KMSDRM_gbm_surface_lock_front_buffer(blitter->gs);
            if (!blitter->next_bo) {
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Could not lock front buffer on GBM surface");
                return 0;
            }

            KMSDRM_Post_gbm_bo(_this, windata, dispdata, viddata, blitter->bo, blitter->next_bo);
        }
        else
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Sync %p failed.", current->fence);
        }
    }

    for (i = 0; i < sizeof(blitter->planes) / sizeof(blitter->planes[0]); i++) {
        KMSDRM_Blitter_FreeTexture(_this, blitter, &blitter->planes[!blitter->next]);
    }

    /* Execution is done, teardown the allocated resources */
    blitter->eglDestroySurface(blitter->egl_display, blitter->egl_surface);
    blitter->eglDestroyContext(blitter->egl_display, blitter->gl_context);
    blitter->eglReleaseThread();
    if (blitter->next_bo) KMSDRM_gbm_surface_release_buffer(blitter->gs, blitter->next_bo);
    if (blitter->bo) KMSDRM_gbm_surface_release_buffer(blitter->gs, blitter->bo);
    KMSDRM_gbm_surface_destroy(blitter->gs);

    /* Signal thread done */
    SDL_UnlockMutex(blitter->mutex);
    return 0;
}

void KMSDRM_BlitterInit(KMSDRM_Blitter *blitter)
{
    if (!blitter)
        return;
    
    blitter->mutex = SDL_CreateMutex();
    blitter->cond = SDL_CreateCond();
    blitter->thread = SDL_CreateThread(KMSDRM_BlitterThread, "KMSDRM_BlitterThread", blitter);
}

void KMSDRM_BlitterQuit(KMSDRM_Blitter *blitter)
{
    /* Flag a stop request */
    SDL_LockMutex(blitter->mutex);
    blitter->thread_stop = 1;

    /* Signal thread in order to perform stop */
    SDL_CondSignal(blitter->cond);
    SDL_UnlockMutex(blitter->mutex);

    /* Wait and perform teardown */
    SDL_WaitThread(blitter->thread, NULL);
    blitter->thread = NULL;
    SDL_DestroyMutex(blitter->mutex);
    SDL_DestroyCond(blitter->cond);
}

#endif /* SDL_VIDEO_OPENGL_EGL */