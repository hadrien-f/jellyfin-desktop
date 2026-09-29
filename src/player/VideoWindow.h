#ifndef VIDEOWINDOW_H
#define VIDEOWINDOW_H

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QWindow>

class MpvController;
class QThread;
struct mpv_render_context;

///////////////////////////////////////////////////////////////////////////////////////////////////
// The application's window: mpv's video with the Qt Quick scene (the web client) on top.
//
// Each frame, mpv renders the video into the window through the libmpv OpenGL render
// API, then the scene is blended on top. This window also owns mpv (MpvController).
//
// The scene is rendered offscreen by a QQuickRenderControl whose renderWindow() is
// this window, rather than living in its own child window: Qt Quick only gives an
// item keyboard focus when its window is the application's focus window or is
// rendered for it (windowHasFocus() in qquickdeliveryagent.cpp), and on Wayland the
// focus window is always the top-level surface. Input arriving here is forwarded
// to the scene. The scene is rendered only when it changes.
//
// HDR (isHdr()): on Wayland, the surface asks for BT.2020 / PQ (BT.2100) through
// QSurfaceFormat, which Qt Wayland sends to the compositor as a wp_color_manager_v1
// image description. When the compositor accepts it, mpv renders in PQ
// (target-trc=pq, target-prim=bt.2020) and the scene is converted from sRGB to PQ,
// at the SDR white level (setSdrWhiteLevel()), into a half-float texture. Otherwise
// (other platforms, compositors without colour management) everything is sRGB and
// the scene texture is blended as is.
//
// Limitations:
//  - The colour space is fixed when the window is created, so SDR media is mapped
//    into PQ by mpv, and the compositor tone-maps when the output is SDR.
//  - Qt sends primaries and transfer function only, no mastering luminance or
//    MaxCLL (qwaylandcolormanagement.cpp, Qt 6.10).
//  - mpv's render API uses the legacy "gpu" renderer, which can't render Dolby
//    Vision profile 5. Passing MPV_RENDER_PARAM_BACKEND "gpu-next" will lift this
//    once mpv ships a gpu-next render backend (mpv PR #16818).
class VideoWindow : public QWindow
{
  Q_OBJECT

public:
  VideoWindow();
  ~VideoWindow() override;

  // Creates the platform window, the OpenGL resources and mpv. Returns false if
  // OpenGL isn't usable.
  bool initialize();

  // Whether the surface is BT.2100 PQ (see the class comment).
  bool isHdr() const { return m_hdr; }

  MpvController* mpvController() { return m_mpv; }

  // The offscreen window the scene is rendered in.
  QQuickWindow* sceneWindow() { return &m_scene; }

  // Takes ownership of the scene's root item and sizes it to the window.
  void setSceneRoot(QQuickItem* root);

  // Starts rendering mpv's video. Call once mpv's options are set.
  void startVideo();

  // HDR only: luminance of the scene's white, in cd/m². Defaults to 203, the BT.2408
  // reference white; the compositor's SDR white level is the better value when known.
  void setSdrWhiteLevel(qreal nits);

protected:
  bool event(QEvent* event) override;
  void exposeEvent(QExposeEvent* event) override;

private:
  class RenderControl : public QQuickRenderControl
  {
  public:
    explicit RenderControl(QWindow* window) : m_window(window) {}
    QWindow* renderWindow(QPoint* offset) override;

  private:
    QWindow* m_window;
  };

  void stopVideo();
  void render();
  void renderScene(const QSize& size);
  void convertScene(const QSize& size);
  void scheduleSceneUpdate();
  static void onMpvUpdate(void* context);

  QOpenGLContext m_context;          // this window's context: mpv and blending
  QOpenGLContext m_sceneContext;     // Qt Quick's context, same share group
  QOffscreenSurface m_sceneSurface;
  RenderControl m_renderControl;
  QQuickWindow m_scene;
  QQuickItem* m_sceneRoot = nullptr;

  QOpenGLShaderProgram m_convertProgram;
  QOpenGLShaderProgram m_blendProgram;
  QOpenGLVertexArrayObject m_vao;
  GLuint m_sceneTexture = 0;         // sRGB, premultiplied, rendered by Qt Quick
  GLuint m_pqTexture = 0;            // HDR: PQ, premultiplied, blended every frame
  GLuint m_pqFramebuffer = 0;
  QSize m_textureSize;
  bool m_sceneDirty = true;
  qreal m_sdrWhiteLevel = 203;
  bool m_hdr = false;

  QThread* m_mpvThread = nullptr;
  MpvController* m_mpv = nullptr;
  mpv_render_context* m_mpvRender = nullptr;
};

#endif // VIDEOWINDOW_H
