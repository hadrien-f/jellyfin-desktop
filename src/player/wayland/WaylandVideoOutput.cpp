#include "WaylandVideoOutput.h"
#include "WaylandTranslator.h"

#include <cstring>

#include <QDebug>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QQuickWindow>
#include <QtGui/qguiapplication_platform.h>
// QNativeInterface::Private::QWaylandWindow: the only way to get a window's wl_surface.
#include <QtGui/qpa/qplatformwindow_p.h>

#include <wayland-client.h>

// Safety net: how long WAYLAND_DISPLAY may point at the translator when mpv
// neither connects nor ends the file. mpv creates its VO only once the stream
// is opened and probed, which can take several seconds over the network.
static const int s_overrideTimeoutMs = 30000;

///////////////////////////////////////////////////////////////////////////////////////////////////
static wl_display* qtDisplay()
{
  auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
  return app ? app->display() : nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WaylandVideoOutput::compositorSupportsColorManagement()
{
  wl_display* display = qtDisplay();
  if (!display)
    return false;

  // Private queue, so dispatching never runs Qt's own Wayland event handlers.
  wl_event_queue* queue = wl_display_create_queue(display);
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  wl_registry* registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);

  bool found = false;
  static const wl_registry_listener listener = {
    [](void* data, wl_registry*, uint32_t, const char* interface, uint32_t) {
      if (strcmp(interface, "wp_color_manager_v1") == 0)
        *static_cast<bool*>(data) = true;
    },
    [](void*, wl_registry*, uint32_t) {},
  };
  wl_registry_add_listener(registry, &listener, &found);
  wl_display_roundtrip_queue(display, queue);

  wl_registry_destroy(registry);
  wl_event_queue_destroy(queue);
  return found;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
WaylandVideoOutput::WaylandVideoOutput(QQuickWindow* window) : QObject(window), m_window(window)
{
  m_overrideTimeout.setSingleShot(true);
  m_overrideTimeout.setInterval(s_overrideTimeoutMs);
  connect(&m_overrideTimeout, &QTimer::timeout, this, [this]() {
    if (m_override.isArmed())
      qDebug() << "WaylandVideoOutput: no VO connected, restoring WAYLAND_DISPLAY";
    m_override.restore();
  });
}

///////////////////////////////////////////////////////////////////////////////////////////////////
WaylandVideoOutput::~WaylandVideoOutput()
{
  m_override.restore();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WaylandVideoOutput::start()
{
  if (m_translator)
    return m_translator->isValid();

  wl_display* display = qtDisplay();
  if (!display || !m_window)
    return false;

  WaylandTranslator::Callbacks callbacks;
  // Callbacks run on the translator thread; hop to ours.
  callbacks.shellBound = [this]() {
    QMetaObject::invokeMethod(this, [this]() {
      m_overrideTimeout.stop();
      m_override.restore();
    }, Qt::QueuedConnection);
  };
  callbacks.videoSurfaceChanged = [this](bool attached) {
    QMetaObject::invokeMethod(this, [this, attached]() { onVideoSurfaceChanged(attached); },
                              Qt::QueuedConnection);
  };
  callbacks.parentCommitNeeded = [this]() {
    QMetaObject::invokeMethod(this, [this]() {
      if (m_window)
        m_window->update();
    }, Qt::QueuedConnection);
  };
  m_translator = std::make_unique<WaylandTranslator>(display, std::move(callbacks));
  if (!m_translator->isValid())
    return false;

  // Qt destroys the wl_surface when the window is hidden and emits
  // surfaceDestroyed only afterwards; visibleChanged(false) and
  // SurfaceAboutToBeDestroyed come before, so detach there.
  connect(m_window, &QWindow::visibleChanged, this, [this](bool visible) {
    if (!visible)
      m_translator->setParentSurface(nullptr);
  }, Qt::DirectConnection);
  m_window->installEventFilter(this);

  if (auto* wlWindow = m_window->nativeInterface<QNativeInterface::Private::QWaylandWindow>())
    connect(wlWindow, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, this,
            &WaylandVideoOutput::updateParentSurface);

  connect(m_window, &QWindow::widthChanged, this, &WaylandVideoOutput::updateGeometry);
  connect(m_window, &QWindow::heightChanged, this, &WaylandVideoOutput::updateGeometry);

  updateGeometry();
  updateParentSurface();
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WaylandVideoOutput::prepareForPlayback()
{
  if (!start())
    return false;

  // An attached VO is reused for the next file; nothing reconnects.
  if (m_videoAttached)
    return true;

  m_override.arm(m_translator->socketName());
  m_overrideTimeout.start();
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WaylandVideoOutput::playbackEnded()
{
  // The file ended (or failed) before mpv created a VO.
  m_overrideTimeout.stop();
  m_override.restore();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool WaylandVideoOutput::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == m_window && event->type() == QEvent::PlatformSurface &&
      static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
        QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
    m_translator->setParentSurface(nullptr);

  return QObject::eventFilter(watched, event);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WaylandVideoOutput::updateParentSurface()
{
  auto* wlWindow = m_window ? m_window->nativeInterface<QNativeInterface::Private::QWaylandWindow>()
                            : nullptr;
  m_translator->setParentSurface(wlWindow ? wlWindow->surface() : nullptr);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WaylandVideoOutput::updateGeometry()
{
  if (!m_window)
    return;

  // Logical size: mpv renders at the output scale it gets from the compositor.
  m_translator->configure(m_window->width(), m_window->height());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WaylandVideoOutput::onVideoSurfaceChanged(bool attached)
{
  if (attached == m_videoAttached)
    return;

  m_videoAttached = attached;
  // Black while browsing, so no stale video or desktop shows behind the UI,
  // but never fully opaque: for an opaque colour QQuickWindow::setColor()
  // drops the alpha channel from the window format, Qt Wayland then declares
  // the whole surface opaque on its next geometry change (and never clears it
  // again), and the compositor may present the window's buffer on its own,
  // without the video below.
  if (m_window)
    m_window->setColor(attached ? QColor(Qt::transparent) : QColor(0, 0, 0, 254));

  emit videoAttachedChanged(attached);
}
