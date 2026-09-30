#include "VideoWindow.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QOpenGLExtraFunctions>
#include <QPlatformSurfaceEvent>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderTarget>
#include <QThread>
#include <private/qwindow_p.h>

#include <MpvController>

#include <mpv/render_gl.h>

// Full-window quad from gl_VertexID, drawn as a 4-vertex triangle strip.
static const char* s_vertexShader = R"(
out vec2 uv;
void main()
{
  vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Premultiplied sRGB scene → premultiplied BT.2020 / PQ.
//  - sRGB content is decoded with a 2.2 power, as KWin and Qt Wayland treat it.
//  - BT.709 → BT.2020 primaries: ITU-R BT.2087, table 2.
//  - PQ inverse EOTF constants: SMPTE ST 2084 / ITU-R BT.2100, table 4.
// Blending then happens on PQ-encoded values, which is fine for UI (browsers blend
// in gamma space too).
static const char* s_convertShader = R"(
in vec2 uv;
out vec4 color;
uniform sampler2D scene;
uniform float whiteLevel; // scene white in PQ's linear range: cd/m² / 10000
void main()
{
  vec4 c = texture(scene, uv);
  if (c.a <= 0.0)
  {
    color = vec4(0.0);
    return;
  }
  vec3 linear709 = pow(clamp(c.rgb / c.a, 0.0, 1.0), vec3(2.2));
  const mat3 bt709ToBt2020 = mat3(0.6274, 0.0691, 0.0164,  // column-major
                                  0.3293, 0.9195, 0.0880,
                                  0.0433, 0.0114, 0.8956);
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 4096.0 * 128.0;
  const float c1 = 3424.0 / 4096.0;
  const float c2 = 2413.0 / 4096.0 * 32.0;
  const float c3 = 2392.0 / 4096.0 * 32.0;
  vec3 y = pow(bt709ToBt2020 * linear709 * whiteLevel, vec3(m1));
  vec3 pq = pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
  color = vec4(pq * c.a, c.a);
}
)";

static const char* s_blendShader = R"(
in vec2 uv;
out vec4 color;
uniform sampler2D scene;
void main()
{
  color = texture(scene, uv);
}
)";

///////////////////////////////////////////////////////////////////////////////////////////////////
QWindow* VideoWindow::RenderControl::renderWindow(QPoint* offset)
{
  if (offset)
    *offset = QPoint(0, 0);
  return m_window;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
VideoWindow::VideoWindow() : m_renderControl(this), m_scene(&m_renderControl)
{
  setSurfaceType(QSurface::OpenGLSurface);

  // The default format carries the platform's OpenGL version (OpenGLDetect.cpp).
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  // Only Qt Wayland turns a colour space into a compositor colour description.
  if (QGuiApplication::platformName() == "wayland")
  {
    format.setRedBufferSize(10);
    format.setGreenBufferSize(10);
    format.setBlueBufferSize(10);
    format.setColorSpace(QColorSpace(QColorSpace::Primaries::Bt2020,
                                     QColorSpace::TransferFunction::St2084));
  }
  setFormat(format);

  m_scene.setColor(Qt::transparent);

  // mpv's handle lives on a worker thread, as in MpvQt's MpvAbstractItem.
  m_mpvThread = new QThread;
  m_mpv = new MpvController;
  m_mpvThread->start();
  m_mpv->moveToThread(m_mpvThread);
  QMetaObject::invokeMethod(m_mpv, &MpvController::init, Qt::BlockingQueuedConnection);
  connect(m_mpvThread, &QThread::finished, m_mpv, &MpvController::deleteLater);
#ifdef Q_OS_WIN32
  // Force desktop OpenGL and disable advanced features for compatibility with older GPUs
  m_mpv->setProperty("gpu-api", "opengl");
  m_mpv->setProperty("opengl-es", "no");
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
VideoWindow::~VideoWindow()
{
  stopVideo();

  m_sceneContext.makeCurrent(&m_sceneSurface);
  delete m_sceneRoot;
  m_renderControl.invalidate();

  // Skipped when the window was closed: its surface is gone (the PlatformSurface event
  // already freed mpv's render context). Otherwise the context stays current for the
  // shader programs' destructors.
  if (m_context.makeCurrent(this))
  {
    auto* f = m_context.extraFunctions();
    f->glDeleteFramebuffers(1, &m_pqFramebuffer);
    f->glDeleteTextures(1, &m_pqTexture);
    f->glDeleteTextures(1, &m_sceneTexture);
  }

  // After the render context: mpv requires it gone first (render.h).
  mpv_handle* mpv = m_mpv->mpv();
  mpv_set_wakeup_callback(mpv, nullptr, nullptr);
  m_mpvThread->quit();
  m_mpvThread->wait();
  delete m_mpvThread;
  mpv_terminate_destroy(mpv);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool VideoWindow::initialize()
{
  create();
  // Qt Wayland keeps the colour space only if the compositor accepted it.
  const QColorSpace requested = requestedFormat().colorSpace();
  m_hdr = requested.transferFunction() == QColorSpace::TransferFunction::St2084 &&
          format().colorSpace() == requested;
  if (!m_hdr && requested.transferFunction() == QColorSpace::TransferFunction::St2084)
  {
    // The 10-bit request was for PQ only. Its EGL configs have no usable alpha, which the
    // shadows of Qt's own decorations need (GNOME): they would be drawn opaque black.
    destroy();
    setFormat(QSurfaceFormat::defaultFormat());
    create();
  }

  m_context.setFormat(requestedFormat());
  // QtWebEngine shares its textures through the global share context.
  m_context.setShareContext(QOpenGLContext::globalShareContext());
  // Qt Quick gets its own context, with the scene window's format: its RHI makes
  // the context current on an offscreen surface of that format
  // (qsgrhisupport.cpp), which the 10-bit window config can't provide.
  m_sceneContext.setFormat(m_scene.requestedFormat());
  m_sceneContext.setShareContext(QOpenGLContext::globalShareContext());
  if (!m_context.create() || !m_sceneContext.create())
  {
    qWarning() << "VideoWindow: failed to create the OpenGL contexts";
    return false;
  }
  m_sceneSurface.setFormat(m_sceneContext.format());
  m_sceneSurface.create();

  if (!m_context.makeCurrent(this))
    return false;
  const QByteArray version = m_context.isOpenGLES() ? "#version 300 es\nprecision highp float;\n"
                                                    : "#version 140\n";
  m_convertProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, version + s_vertexShader);
  m_convertProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, version + s_convertShader);
  m_blendProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, version + s_vertexShader);
  m_blendProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, version + s_blendShader);
  if (!m_convertProgram.link() || !m_blendProgram.link())
  {
    qWarning() << "VideoWindow: shaders:" << m_convertProgram.log() << m_blendProgram.log();
    return false;
  }
  m_vao.create();

  m_sceneContext.makeCurrent(&m_sceneSurface);
  m_scene.setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(&m_sceneContext));
  if (!m_renderControl.initialize())
  {
    qWarning() << "VideoWindow: failed to initialize Qt Quick rendering";
    return false;
  }
  connect(&m_renderControl, &QQuickRenderControl::renderRequested,
          this, &VideoWindow::scheduleSceneUpdate);
  connect(&m_renderControl, &QQuickRenderControl::sceneChanged,
          this, &VideoWindow::scheduleSceneUpdate);

  qInfo() << "VideoWindow:" << (m_hdr ? "HDR (BT.2100 PQ)" : "SDR (sRGB)") << "output";
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::setSceneRoot(QQuickItem* root)
{
  delete m_sceneRoot;
  m_sceneRoot = root;
  if (!root)
    return;
  root->setParentItem(m_scene.contentItem());
  // Also as a QObject child, so the scene window's findChild() sees the items.
  root->setParent(m_scene.contentItem());
  root->setSize(size());
  scheduleSceneUpdate();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::startVideo()
{
  if (m_mpvRender || !m_context.makeCurrent(this))
    return;

  mpv_opengl_init_params gl{
    [](void*, const char* name) -> void* {
      return reinterpret_cast<void*>(QOpenGLContext::currentContext()->getProcAddress(name));
    },
    nullptr};
  // Hardware decoding interop needs the application's display connection (render.h).
  mpv_render_param display{MPV_RENDER_PARAM_INVALID, nullptr};
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
  if (auto* x11 = qApp->nativeInterface<QNativeInterface::QX11Application>())
    display = {MPV_RENDER_PARAM_X11_DISPLAY, x11->display()};
  else if (auto* wayland = qApp->nativeInterface<QNativeInterface::QWaylandApplication>())
    display = {MPV_RENDER_PARAM_WL_DISPLAY, wayland->display()};
#endif
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
    display,
    {MPV_RENDER_PARAM_INVALID, nullptr}};
  int err = mpv_render_context_create(&m_mpvRender, m_mpv->mpv(), params);
  if (err < 0)
  {
    qWarning() << "VideoWindow: mpv_render_context_create:" << mpv_error_string(err);
    m_mpvRender = nullptr;
    return;
  }
  mpv_render_context_set_update_callback(m_mpvRender, &VideoWindow::onMpvUpdate, this);
  requestUpdate();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::stopVideo()
{
  if (!m_mpvRender || !m_context.makeCurrent(this))
    return;
  mpv_render_context_set_update_callback(m_mpvRender, nullptr, nullptr);
  mpv_render_context_free(m_mpvRender);
  m_mpvRender = nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::setSdrWhiteLevel(qreal nits)
{
  if (nits <= 0 || qFuzzyCompare(nits, m_sdrWhiteLevel))
    return;
  m_sdrWhiteLevel = nits;
  scheduleSceneUpdate();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::onMpvUpdate(void* context)
{
  // Called on mpv's threads: only schedule, never call mpv from here (render.h).
  QMetaObject::invokeMethod(static_cast<VideoWindow*>(context), &QWindow::requestUpdate,
                            Qt::QueuedConnection);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::scheduleSceneUpdate()
{
  m_sceneDirty = true;
  requestUpdate();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::exposeEvent(QExposeEvent*)
{
  // Render now, as Qt's OpenGL window examples do, rather than requestUpdate(): when a
  // frame callback times out, Qt Wayland marks the window unexposed and later drops the
  // pending update request, and QWindow doesn't request again while one is pending.
  // Waiting for it stalls rendering for good (seen after switching the display to HDR).
  if (isExposed() && m_sceneRoot)
    render();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool VideoWindow::event(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::UpdateRequest:
      if (isExposed() && m_sceneRoot)
        render();
      return true;

    case QEvent::PlatformSurface:
      // Closing the window destroys its surface before ~VideoWindow runs: free mpv's
      // render context while the GL context can still be made current on it.
      if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
          QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        stopVideo();
      return QWindow::event(event);

    // Input arrives at this window; the scene is what handles it. The scene covers
    // the window at (0, 0), so positions need no mapping.
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::ShortcutOverride:
    case QEvent::InputMethod:
    case QEvent::InputMethodQuery:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
    case QEvent::Wheel:
    case QEvent::Enter:
    case QEvent::Leave:
      return QCoreApplication::sendEvent(&m_scene, event);

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    {
      // As in Qt's rendercontrol example: rebuild the event so that its scene
      // position is its window position.
      auto* e = static_cast<QMouseEvent*>(event);
      QMouseEvent mapped(e->type(), e->position(), e->globalPosition(), e->button(), e->buttons(),
                         e->modifiers(), e->pointingDevice());
      // Click counting (double clicks in QtWebEngine) compares timestamps.
      mapped.setTimestamp(e->timestamp());
      return QCoreApplication::sendEvent(&m_scene, &mapped);
    }

    default:
      return QWindow::event(event);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::render()
{
  const QSize size = (QSizeF(this->size()) * devicePixelRatio()).toSize();
  if (size.isEmpty())
    return;
  syncSceneDevicePixelRatio();
  if (m_sceneDirty || size != m_textureSize)
    renderScene(size);

  if (!m_context.makeCurrent(this))
    return;
  auto* f = m_context.extraFunctions();
  if (m_sceneDirty)
  {
    if (m_hdr)
      convertScene(size);
    m_textureSize = size;
    m_sceneDirty = false;
  }

  f->glBindFramebuffer(GL_FRAMEBUFFER, m_context.defaultFramebufferObject());
  f->glViewport(0, 0, size.width(), size.height());
  if (m_mpvRender)
  {
    // Must run before each render (render.h).
    mpv_render_context_update(m_mpvRender);
    mpv_opengl_fbo fbo{int(m_context.defaultFramebufferObject()), size.width(), size.height(),
                       m_hdr ? GL_RGB10_A2 : 0};
    int flipY = 1;
    mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flipY},
      {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context_render(m_mpvRender, params);
  }
  else
  {
    f->glClearColor(0, 0, 0, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);
  }

  // mpv leaves its own state behind: set everything this pass relies on. That includes the
  // framebuffer: mpv leaves 0 bound, while Qt's is its own FBO when Qt draws the window
  // decorations (GNOME), and anything drawn into 0 is overwritten by Qt's copy.
  f->glBindFramebuffer(GL_FRAMEBUFFER, m_context.defaultFramebufferObject());
  f->glViewport(0, 0, size.width(), size.height());
  f->glDisable(GL_DEPTH_TEST);
  f->glDisable(GL_SCISSOR_TEST);
  f->glEnable(GL_BLEND);
  f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  f->glActiveTexture(GL_TEXTURE0);
  f->glBindTexture(GL_TEXTURE_2D, m_hdr ? m_pqTexture : m_sceneTexture);
  m_blendProgram.bind();
  m_blendProgram.setUniformValue("scene", 0);
  m_vao.bind();
  f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  m_vao.release();
  m_blendProgram.release();
  f->glDisable(GL_BLEND);

  m_context.swapBuffers(this);
  if (m_mpvRender)
    mpv_render_context_report_swap(m_mpvRender);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// QtWebEngine takes its scale from QWindow::devicePixelRatio() of the scene window, not from
// QQuickWindow::effectiveDevicePixelRatio() as Qt documents for QQuickRenderControl
// (QTBUG-151048). The scene window is never shown, so Qt gives it the screen's integer scale:
// at a fractional scale (GNOME at 175 %) the page renders at 2x and is downscaled, blurring
// text. Give it this window's ratio instead, through private API, until that bug is fixed.
// Qt resets the value when the scene window's screen changes; render() calls this every frame.
void VideoWindow::syncSceneDevicePixelRatio()
{
  QWindowPrivate* scene = QWindowPrivate::get(&m_scene);
  if (scene->devicePixelRatio == devicePixelRatio())
    return;
  scene->devicePixelRatio = devicePixelRatio();
  QEvent event(QEvent::DevicePixelRatioChange);
  QCoreApplication::sendEvent(&m_scene, &event);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::renderScene(const QSize& size)
{
  if (!m_sceneContext.makeCurrent(&m_sceneSurface))
    return;
  auto* f = m_sceneContext.extraFunctions();

  if (size != m_textureSize)
  {
    f->glDeleteTextures(1, &m_sceneTexture);
    f->glGenTextures(1, &m_sceneTexture);
    f->glBindTexture(GL_TEXTURE_2D, m_sceneTexture);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, nullptr);
    QQuickRenderTarget target = QQuickRenderTarget::fromOpenGLTexture(m_sceneTexture, size);
    target.setDevicePixelRatio(devicePixelRatio());
    m_scene.setRenderTarget(target);
    m_scene.setGeometry(0, 0, width(), height());
    m_sceneRoot->setSize(this->size());
    m_sceneDirty = true;
  }

  m_renderControl.beginFrame();
  m_renderControl.polishItems();
  m_renderControl.sync();
  m_renderControl.render();
  m_renderControl.endFrame();
  // The window's context samples the texture next.
  // ponytail: glFinish on every scene change; a fence sync if it ever costs frames.
  f->glFinish();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void VideoWindow::convertScene(const QSize& size)
{
  auto* f = m_context.extraFunctions();

  if (size != m_textureSize)
  {
    f->glDeleteFramebuffers(1, &m_pqFramebuffer);
    f->glDeleteTextures(1, &m_pqTexture);
    f->glGenTextures(1, &m_pqTexture);
    f->glBindTexture(GL_TEXTURE_2D, m_pqTexture);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Half float: RGB10_A2 would leave 2 bits of alpha for the translucent UI.
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, size.width(), size.height(), 0, GL_RGBA,
                    GL_HALF_FLOAT, nullptr);
    f->glGenFramebuffers(1, &m_pqFramebuffer);
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_pqFramebuffer);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_pqTexture, 0);
  }

  f->glBindFramebuffer(GL_FRAMEBUFFER, m_pqFramebuffer);
  f->glViewport(0, 0, size.width(), size.height());
  f->glDisable(GL_BLEND);
  f->glDisable(GL_DEPTH_TEST);
  f->glDisable(GL_SCISSOR_TEST);
  f->glActiveTexture(GL_TEXTURE0);
  f->glBindTexture(GL_TEXTURE_2D, m_sceneTexture);
  m_convertProgram.bind();
  m_convertProgram.setUniformValue("scene", 0);
  m_convertProgram.setUniformValue("whiteLevel", float(m_sdrWhiteLevel / 10000.0));
  m_vao.bind();
  f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  m_vao.release();
  m_convertProgram.release();
}
