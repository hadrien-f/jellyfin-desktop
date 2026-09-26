#include "MpvVideoItem.h"
#include "PlayerComponent.h"
#include <MpvController>
#include <QDebug>

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : MpvAbstractItem(parent)
{
    qDebug() << "MpvVideoItem constructed";
    if (PlayerComponent::Get().videoBackend() == VideoBackend::WaylandSubsurface) {
        // mpv's own Vulkan VO, shown below the web view by WaylandVideoOutput.
        // gpu-next tags its swapchain with the video's colour space (PQ/HLG for
        // HDR) when the compositor supports it, and tone-maps otherwise.
        Q_EMIT setProperty("vo", "gpu-next");
        Q_EMIT setProperty("gpu-api", "vulkan");
        Q_EMIT setProperty("gpu-context", "waylandvk");
        Q_EMIT setProperty("target-colorspace-hint", "yes");
        // The VO fills the window; as fullscreen it takes the size it is given.
        Q_EMIT setProperty("fullscreen", "yes");
        // The item only hosts the mpv handle. Without content the scene graph
        // never asks for its renderer, so no OpenGL render context is created.
        setFlag(QQuickItem::ItemHasContents, false);
        setVisible(false);
        return;
    }

    // Critical: Set vo=libmpv for Qt integration
    Q_EMIT setProperty("vo", "libmpv");

#ifdef Q_OS_WIN32
    // Force desktop OpenGL and disable advanced features for compatibility with older GPUs
    Q_EMIT setProperty("gpu-api", "opengl");
    Q_EMIT setProperty("opengl-es", "no");
#endif
}

void MpvVideoItem::setPlayerComponent(PlayerComponent* player)
{
    qDebug() << "MpvVideoItem::setPlayerComponent called, mpvController():" << mpvController();
    m_player = player;

    // When mpv is ready, give controller to PlayerComponent
    connect(this, &MpvAbstractItem::ready, this, [this]() {
        qDebug() << "MpvVideoItem ready() signal fired!";
        if (m_player && mpvController()) {
            qDebug() << "Setting mpv controller and initializing";
            m_player->setMpvController(mpvController());
            m_player->initializeMpv();
        } else {
            qWarning() << "ready() fired but m_player:" << m_player << "mpvController():" << mpvController();
        }
    });

    // Check if already ready
    if (mpvController()) {
        qDebug() << "MpvVideoItem already ready, initializing now";
        m_player->setMpvController(mpvController());
        m_player->initializeMpv();
    } else {
        qDebug() << "MpvVideoItem not ready yet, waiting for ready() signal";
    }
}
