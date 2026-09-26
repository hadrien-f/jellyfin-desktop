#include "WaylandDisplayOverride.h"

#include <QtGlobal>

static const char* s_variable = "WAYLAND_DISPLAY";

///////////////////////////////////////////////////////////////////////////////////////////////////
void WaylandDisplayOverride::arm(const QByteArray& socket)
{
  if (!m_armed)
  {
    m_hadValue = qEnvironmentVariableIsSet(s_variable);
    m_saved = qgetenv(s_variable);
    m_armed = true;
  }
  qputenv(s_variable, socket);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void WaylandDisplayOverride::restore()
{
  if (!m_armed)
    return;

  if (m_hadValue)
    qputenv(s_variable, m_saved);
  else
    qunsetenv(s_variable);
  m_armed = false;
}
