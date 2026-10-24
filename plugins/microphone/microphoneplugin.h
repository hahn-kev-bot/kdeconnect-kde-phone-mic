/**
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QIODevice>
#include <QObject>
#include <QSharedPointer>
#include <QTimer>

#include <memory>

#include <core/kdeconnectplugin.h>

class AudioOutput;

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
    Q_PROPERTY(bool streaming READ isStreaming NOTIFY streamingChanged)
    Q_PROPERTY(QString outputDevice READ outputDevice NOTIFY streamingChanged)
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY streamingChanged)
    Q_PROPERTY(qlonglong startedAt READ startedAt NOTIFY streamingChanged)
    Q_PROPERTY(QString outputMode READ outputMode NOTIFY streamingChanged)
    Q_PROPERTY(bool exclusiveMode READ exclusiveMode WRITE setExclusiveMode NOTIFY exclusiveModeChanged)

public:
    explicit MicrophonePlugin(QObject *parent, const QVariantList &args);
    ~MicrophonePlugin() override;

    void receivePacket(const NetworkPacket &np) override;
    QString dbusPath() const override;

    Q_SCRIPTABLE bool isStreaming() const;
    Q_SCRIPTABLE void stop();

    QString outputDevice() const;
    int sampleRate() const;

    /**
     * Milliseconds since the epoch at which the current stream started, 0 while idle
     */
    qlonglong startedAt() const;

    /**
     * "exclusive" or "shared" while streaming on Windows, "" while idle
     */
    QString outputMode() const;

    /**
     * Whether the next stream should open the output device in WASAPI exclusive mode (lowest
     * latency). Persisted in the plugin config; a running stream is not affected.
     */
    bool exclusiveMode() const;
    void setExclusiveMode(bool exclusive);

Q_SIGNALS:
    Q_SCRIPTABLE void streamingChanged(bool streaming);
    Q_SCRIPTABLE void exclusiveModeChanged(bool exclusive);

    /**
     * Audio level of the samples played since the previous emission, both in 0..1.
     * Emitted at most every 50 ms while streaming, and once more with zeros when the stream stops.
     */
    Q_SCRIPTABLE void levelChanged(double peak, double rms);

private:
    std::unique_ptr<AudioOutput> openOutput(int sampleRate, int channels, const QString &wantedDevice, QString *chosenDescription);
    void socketReadyRead();
    void drainPending();
    void scanLevels(const char *data, qsizetype size);
    void stopStream();

    QSharedPointer<QIODevice> m_socket;
    std::unique_ptr<AudioOutput> m_output;
    QTimer m_drainTimer;
    QByteArray m_pending;
    qsizetype m_primeBytes = 0;
    qsizetype m_maxPendingBytes = 0;
    qint64 m_bytesPerSecond = 0;
    qint64 m_droppedBytes = 0;
    quint32 m_levelTicks = 0;
    bool m_primed = false;
    QString m_streamId;
    QString m_outputDevice;
    QString m_outputMode;
    int m_sampleRate = 0;
    qlonglong m_startedAt = 0;

    QElapsedTimer m_levelTimer;
    qsizetype m_levelSkip = 0;
    double m_levelPeak = 0.0;
    double m_levelSquares = 0.0;
    qint64 m_levelSamples = 0;
};
