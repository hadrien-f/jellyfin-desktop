#ifndef WAYLANDVIDEOOUTPUT_H
#define WAYLANDVIDEOOUTPUT_H

#include <memory>

#include <QObject>
#include <QPointer>
#include <QTimer>

#include "WaylandDisplayOverride.h"

class QQuickWindow;
class WaylandTranslator;

// Integrates mpv's own video output with the main window on Wayland: mpv's
// window becomes a subsurface below the web view (see WaylandTranslator), so
// the compositor receives the video with its own colour space and HDR metadata.
//
// While a video subsurface is attached the window background is transparent,
// so the video shows through wherever the web view is transparent too.
class WaylandVideoOutput : public QObject
{
  Q_OBJECT
public:
  explicit WaylandVideoOutput(QQuickWindow* window);
  ~WaylandVideoOutput() override;

  // True when the compositor advertises wp_color_manager_v1, i.e. can take
  // HDR surfaces. Must be called with a Wayland QGuiApplication.
  static bool compositorSupportsColorManagement();

  // Call right before a loadfile that may create mpv's VO. Points mpv at the
  // translator until its VO connected. Returns false if the translator could
  // not be started, in which case mpv would open a window of its own.
  bool prepareForPlayback();

  // Call when a file ended or failed; stops pointing new connections at the
  // translator if mpv never connected.
  void playbackEnded();

  bool videoAttached() const { return m_videoAttached; }

Q_SIGNALS:
  void videoAttachedChanged(bool attached);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  bool start();
  void updateParentSurface();
  void updateGeometry();
  void onVideoSurfaceChanged(bool attached);

  QPointer<QQuickWindow> m_window;
  std::unique_ptr<WaylandTranslator> m_translator;
  WaylandDisplayOverride m_override;
  QTimer m_overrideTimeout;
  bool m_videoAttached = false;
};

#endif // WAYLANDVIDEOOUTPUT_H
