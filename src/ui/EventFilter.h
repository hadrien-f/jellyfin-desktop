//
// Created by Tobias Hieta on 07/03/16.
//

#ifndef PLEXMEDIAPLAYER_EVENTFILTER_H
#define PLEXMEDIAPLAYER_EVENTFILTER_H

#include <QObject>
#include <QEvent>

class EventFilter : public QObject
{
  Q_OBJECT
public:
  // parent: the QML root object, which carries webDesktopMode and the "web" item.
  explicit EventFilter(QObject* parent = nullptr) : QObject(parent), m_currentKeyDown(false) {}

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  bool m_currentKeyDown;
};

#endif //PLEXMEDIAPLAYER_EVENTFILTER_H
