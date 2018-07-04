/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "core/plugins.h"
#include "driver/gl/egl_dispatch_table.h"
#include "driver/gl/gl_common.h"

static void *GetEGLHandle()
{
#if ENABLED(RDOC_WIN32)
  return Process::LoadModule(LocatePluginFile("gles", "libEGL.dll").c_str());
#else
  void *handle = Process::LoadModule("libEGL.so");

  if(!handle)
    handle = Process::LoadModule("libEGL.so.1");

  return handle;
#endif
}

class EGLPlatform : public GLPlatform
{
  bool MakeContextCurrent(GLWindowingData data)
  {
    if(EGL.MakeCurrent)
      return EGL.MakeCurrent(data.egl_dpy, data.egl_wnd, data.egl_wnd, data.egl_ctx) == EGL_TRUE;

    return false;
  }

  GLWindowingData MakeContext(GLWindowingData share)
  {
    GLWindowingData ret;

    if(EGL.CreateContext && EGL.ChooseConfig && EGL.CreatePbufferSurface)
    {
      ret = CreateWindowingData(share.egl_dpy, share.ctx, 0);
    }

    return ret;
  }

  void DeleteContext(GLWindowingData context)
  {
    if(context.wnd && EGL.DestroySurface)
      EGL.DestroySurface(context.egl_dpy, context.egl_wnd);

    if(context.ctx && EGL.DestroyContext)
      EGL.DestroyContext(context.egl_dpy, context.egl_ctx);
  }

  void DeleteReplayContext(GLWindowingData context)
  {
    if(EGL.DestroyContext)
    {
      EGL.MakeCurrent(context.egl_dpy, 0L, 0L, NULL);
      EGL.DestroySurface(context.egl_dpy, context.egl_wnd);
      EGL.DestroyContext(context.egl_dpy, context.egl_ctx);
    }
  }

  void SwapBuffers(GLWindowingData context) { EGL.SwapBuffers(context.egl_dpy, context.egl_wnd); }
  void GetOutputWindowDimensions(GLWindowingData context, int32_t &w, int32_t &h)
  {
    // On some Linux systems the surface seems to be context dependant.
    // Thus we need to switch to that context where the surface was created.
    // To avoid any problems because of the context change we'll save the old
    // context information so we can switch back to it after the surface query is done.
    GLWindowingData oldContext;
    oldContext.egl_ctx = EGL.GetCurrentContext();
    oldContext.egl_dpy = EGL.GetCurrentDisplay();
    oldContext.egl_wnd = EGL.GetCurrentSurface(EGL_READ);
    MakeContextCurrent(context);

    EGLBoolean width_ok = EGL.QuerySurface(context.egl_dpy, context.egl_wnd, EGL_WIDTH, &w);
    EGLBoolean height_ok = EGL.QuerySurface(context.egl_dpy, context.egl_wnd, EGL_HEIGHT, &h);

    if(!width_ok || !height_ok)
    {
      RDCGLenum error_code = (RDCGLenum)EGL.GetError();
      RDCWARN("Unable to query the surface size. Error: (0x%x) %s", error_code,
              ToStr(error_code).c_str());
    }

    MakeContextCurrent(oldContext);
  }

  bool IsOutputWindowVisible(GLWindowingData context) { return true; }
  GLWindowingData MakeOutputWindow(WindowingData window, bool depth, GLWindowingData share_context)
  {
    EGLNativeWindowType win = 0;

    switch(window.system)
    {
#if ENABLED(RDOC_WIN32)
      case WindowingSystem::Win32: win = window.win32.window; break;
#elif ENABLED(RDOC_ANDROID)
      case WindowingSystem::Android: win = window.android.window; break;
#elif ENABLED(RDOC_LINUX)
      case WindowingSystem::Xlib: win = window.xlib.window; break;
#endif
      case WindowingSystem::Unknown:
        // allow WindowingSystem::Unknown so that internally we can create a window-less context
        break;
      default: RDCERR("Unexpected window system %u", window.system); break;
    }

    EGLDisplay eglDisplay = EGL.GetDisplay(EGL_DEFAULT_DISPLAY);
    RDCASSERT(eglDisplay);

    return CreateWindowingData(eglDisplay, share_context.ctx, win);
  }

  GLWindowingData CreateWindowingData(EGLDisplay eglDisplay, EGLContext share_ctx,
                                      EGLNativeWindowType window)
  {
    GLWindowingData ret;
    ret.egl_dpy = eglDisplay;
    ret.egl_ctx = NULL;
    ret.egl_wnd = NULL;

    EGLint surfaceType = (window == 0) ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT;
    const EGLint configAttribs[] = {EGL_RED_SIZE,
                                    8,
                                    EGL_GREEN_SIZE,
                                    8,
                                    EGL_BLUE_SIZE,
                                    8,
                                    EGL_RENDERABLE_TYPE,
                                    EGL_OPENGL_ES3_BIT,
                                    EGL_CONFORMANT,
                                    EGL_OPENGL_ES3_BIT,
                                    EGL_SURFACE_TYPE,
                                    surfaceType,
                                    EGL_COLOR_BUFFER_TYPE,
                                    EGL_RGB_BUFFER,
                                    EGL_NONE};

    EGLint numConfigs;
    EGLConfig config;
    if(!EGL.ChooseConfig(eglDisplay, configAttribs, &config, 1, &numConfigs))
    {
      RDCERR("Couldn't find a suitable EGL config");
      return ret;
    }

    // we try and create the highest versioned context we can, but we need at least ES3.0 (and
    // extensions) to function.
    EGLContext ctx = NULL;

    // don't change this ar ray without changing indices in the loop below
    EGLint verAttribs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR,
                           3,
                           EGL_CONTEXT_MINOR_VERSION_KHR,
                           1,
                           EGL_CONTEXT_FLAGS_KHR,
                           EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR,
                           EGL_NONE};

    struct
    {
      int major;
      int minor;
    } versions[] = {
        {3, 2}, {3, 1}, {3, 0},
    };

    for(size_t v = 0; v < ARRAY_COUNT(versions); v++)
    {
      verAttribs[1] = versions[v].major;
      verAttribs[3] = versions[v].minor;
      ctx = EGL.CreateContext(eglDisplay, config, share_ctx, verAttribs);

      if(ctx)
        break;
    }

    // if none of the above worked, try just creating with the client version as 3
    if(!ctx)
    {
      static const EGLint baseAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_FLAGS_KHR,
                                           EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR, EGL_NONE};

      ctx = EGL.CreateContext(eglDisplay, config, share_ctx, baseAttribs);
    }

    if(ctx == NULL)
    {
      RDCERR("Couldn't create GL ES context");
      return ret;
    }

    ret.egl_ctx = ctx;

    EGLSurface surface = 0;
    if(window != 0)
    {
      surface = EGL.CreateWindowSurface(eglDisplay, config, window, NULL);

      if(surface == NULL)
        RDCERR("Couldn't create surface for window");
    }
    else
    {
      static const EGLint pbAttribs[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
      surface = EGL.CreatePbufferSurface(eglDisplay, config, pbAttribs);

      if(surface == NULL)
        RDCERR("Couldn't create a suitable PBuffer");
    }

    ret.egl_wnd = surface;

    return ret;
  }

  bool PopulateForReplay() { return EGL.PopulateForReplay(); }
  ReplayStatus InitialiseAPI(GLWindowingData &replayContext)
  {
    EGL.BindAPI(EGL_OPENGL_ES_API);

    EGLDisplay eglDisplay = EGL.GetDisplay(EGL_DEFAULT_DISPLAY);
    if(!eglDisplay)
    {
      RDCERR("Couldn't open default EGL display");
      return ReplayStatus::APIInitFailed;
    }

    int major, minor;
    EGL.Initialize(eglDisplay, &major, &minor);

    GLWindowingData base;
    base.egl_dpy = eglDisplay;
    base.egl_ctx = EGL_NO_CONTEXT;
    base.egl_wnd = 0;

    replayContext = MakeContext(base);

    if(!replayContext.ctx || !replayContext.wnd)
    {
      RDCERR("Couldn't create OpenGL ES 3.x replay context - required for replay");
      DeleteContext(replayContext);
      RDCEraseEl(replayContext);
      return ReplayStatus::APIHardwareUnsupported;
    }

    return ReplayStatus::Succeeded;
  }

  void *GetReplayFunction(const char *funcname)
  {
    void *ret = (void *)EGL.GetProcAddress(funcname);
    if(ret)
      return ret;

    return Process::GetFunctionAddress(GetEGLHandle(), funcname);
  }
  void DrawQuads(float width, float height, const std::vector<Vec4f> &vertices)
  {
    // legacy quad rendering is not supported on GLES
  }
} eglPlatform;

GLPlatform &GetEGLPlatform()
{
  return eglPlatform;
}

EGLDispatchTable EGL = {};

bool EGLDispatchTable::PopulateForReplay()
{
  RDCASSERT(RenderDoc::Inst().IsReplayApp());

  void *handle = GetEGLHandle();

  if(!handle)
  {
    RDCERR("Can't load libEGL");
    return false;
  }

  RDCDEBUG("Initialising EGL function pointers");

  bool symbols_ok = true;

#define LOAD_FUNC(func, isext)                                                                      \
  if(!this->func)                                                                                   \
    this->func = (CONCAT(PFN_egl, func))Process::GetFunctionAddress(handle, "egl" STRINGIZE(func)); \
  if(!this->func && CheckConstParam(isext))                                                         \
    this->func = (CONCAT(PFN_egl, func)) this->GetProcAddress("egl" STRINGIZE(func));               \
                                                                                                    \
  if(!this->func && !CheckConstParam(isext))                                                        \
  {                                                                                                 \
    symbols_ok = false;                                                                             \
    RDCWARN("Unable to load '%s'", STRINGIZE(func));                                                \
  }

  EGL_HOOKED_SYMBOLS(LOAD_FUNC)
  EGL_NONHOOKED_SYMBOLS(LOAD_FUNC)

#undef LOAD_FUNC
  return symbols_ok;
}