/**
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QObject>
#include <QSharedPointer>
#include <QTimer>

#include <core/kdeconnectplugin.h>

class QAudioSink;

#define PACKET_TYPE_MICROPHONE QStringLiteral("kdeconnect.microphone")
#define PACKET_TYPE_MICROPHONE_REQUEST QStringLiteral("kdeconnect.microphone.request")

/**
 * Plays the raw PCM payload of a kdeconnect.microphone packet on an audio output device.
 * The intended output is a virtual cable ("CABLE Input") so that other applications can use
 * its other end as their microphone. See docs/protocol.md in the phone-mic workspace.
 */
class MicrophonePlugin : public KdeConnectPlugin
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.microphone")

public:
    explicit MicrophonePlugin(QObject *parent, const QVariantList &args);
    ~MicrophonePlugin() override;

    void receivePacket(const NetworkPacket &np) override;
    QString dbusPath() const override;

    Q_SCRIPTABLE bool isStreaming() const;
    Q_SCRIPTABLE void stop();

Q_SIGNALS:
    Q_SCRIPTABLE void streamingChanged(bool streaming);

private:
    void socketReadyRead();
    void drainPending();
    void stopStream();

    QSharedPointer<QIODevice> m_socket;
    QAudioSink *m_sink = nullptr;
    QIODevice *m_sinkDevice = nullptr;
    QTimer m_drainTimer;
    QByteArray m_pending;
    qsizetype m_primeBytes = 0;
    bool m_primed = false;
    QString m_streamId;
};
