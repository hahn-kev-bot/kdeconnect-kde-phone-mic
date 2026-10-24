/**
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "microphoneplugin.h"

#include <KPluginFactory>

#include <QAbstractSocket>
#include <QDateTime>
#include <QDebug>
#include <QtEndian>

#include <cmath>

#include <core/device.h>

#include "plugin_microphone_debug.h"
#include "qtaudiooutput.h"
#ifdef Q_OS_WIN
#include "wasapiaudiooutput.h"
#endif

K_PLUGIN_CLASS_WITH_JSON(MicrophonePlugin, "kdeconnect_microphone.json")

// Microseconds of audio to accumulate before playback starts.
static const qint64 PRIME_DURATION = 20000;
static const int DRAIN_INTERVAL_MS = 10;
// If the network delivers more than this ahead of playback (a burst after connecting, a Wi-Fi hiccup
// catching up), drop the oldest audio so the delay never grows beyond it.
static const qint64 MAX_PENDING_DURATION = 20000;
static const qint64 LEVEL_INTERVAL_MS = 50;
#define CONFIG_EXCLUSIVE_MODE QStringLiteral("exclusiveMode")

MicrophonePlugin::MicrophonePlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
{
    m_drainTimer.setInterval(DRAIN_INTERVAL_MS);
    connect(&m_drainTimer, &QTimer::timeout, this, &MicrophonePlugin::drainPending);
}

MicrophonePlugin::~MicrophonePlugin()
{
    stopStream();
}

std::unique_ptr<AudioOutput> MicrophonePlugin::openOutput(int sampleRate, int channels, const QString &wantedDevice, QString *chosenDescription)
{
#ifdef Q_OS_WIN
    auto wasapi = std::make_unique<WasapiAudioOutput>(static_cast<int>(MAX_PENDING_DURATION / 1000));
    wasapi->setExclusive(exclusiveMode());
    if (wasapi->start(sampleRate, channels, wantedDevice, chosenDescription)) {
        return wasapi;
    }
    qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI output unavailable, falling back to QAudioSink";
#endif
    auto qt = std::make_unique<QtAudioOutput>();
    if (qt->start(sampleRate, channels, wantedDevice, chosenDescription)) {
        return qt;
    }
    return nullptr;
}

void MicrophonePlugin::receivePacket(const NetworkPacket &np)
{
    if (np.type() != PACKET_TYPE_MICROPHONE) {
        return;
    }
    if (!np.hasPayload() || !np.payload()) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "Received a microphone packet without a payload, ignoring";
        return;
    }

    const int sampleRate = np.get<int>(QStringLiteral("sampleRate"), 48000);
    const int channels = np.get<int>(QStringLiteral("channels"), 1);
    const QString formatName = np.get<QString>(QStringLiteral("format"), QStringLiteral("pcm_s16le"));
    const QString streamId = np.get<QString>(QStringLiteral("streamId"));

    if (formatName != QLatin1String("pcm_s16le")) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "Unsupported audio format" << formatName << ", closing stream" << streamId;
        np.payload()->close();
        return;
    }
    if (sampleRate <= 0 || channels <= 0 || channels > 8) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "Invalid audio format" << sampleRate << "Hz," << channels << "channels, closing stream" << streamId;
        np.payload()->close();
        return;
    }

    if (isStreaming()) {
        qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "Stream" << streamId << "replaces active stream" << m_streamId;
        stopStream();
    }

    const QString wantedDevice = config()->getString(QStringLiteral("outputDevice"), QStringLiteral("CABLE Input"));
    QString chosenDevice;
    m_output = openOutput(sampleRate, channels, wantedDevice, &chosenDevice);
    if (!m_output) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "Could not open any audio output, closing stream" << streamId;
        np.payload()->close();
        return;
    }

    m_socket = np.payload();
    m_streamId = streamId;
    m_pending.clear();
    m_primed = false;
    m_bytesPerSecond = static_cast<qint64>(sampleRate) * channels * 2;
    m_primeBytes = static_cast<qsizetype>(m_bytesPerSecond * PRIME_DURATION / 1000000);
    m_maxPendingBytes = static_cast<qsizetype>(m_bytesPerSecond * MAX_PENDING_DURATION / 1000000);
    m_droppedBytes = 0;
    m_outputDevice = chosenDevice;
    m_outputMode = m_output->mode();
    m_sampleRate = sampleRate;
    m_startedAt = QDateTime::currentMSecsSinceEpoch();
    m_levelSkip = 0;
    m_levelPeak = 0.0;
    m_levelSquares = 0.0;
    m_levelSamples = 0;
    m_levelTimer.start();

    // The payload socket may still be connecting at this point; readyRead simply fires once data arrives.
    connect(m_socket.data(), &QIODevice::readyRead, this, &MicrophonePlugin::socketReadyRead);
    connect(m_socket.data(), &QIODevice::readChannelFinished, this, &MicrophonePlugin::stopStream);
    if (auto *socket = qobject_cast<QAbstractSocket *>(m_socket.data())) {
        connect(socket, &QAbstractSocket::disconnected, this, &MicrophonePlugin::stopStream);
        connect(socket, &QAbstractSocket::errorOccurred, this, &MicrophonePlugin::stopStream);
    }
    m_drainTimer.start();

    qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "Streaming" << streamId << "at" << sampleRate << "Hz," << channels << "channels to" << chosenDevice << "(" << m_outputMode
                                          << ")";
    Q_EMIT streamingChanged(true);

    if (m_socket->bytesAvailable() > 0) {
        socketReadyRead();
    }
}

void MicrophonePlugin::socketReadyRead()
{
    if (!m_socket) {
        return;
    }
    m_pending.append(m_socket->readAll());
    if (m_maxPendingBytes > 0 && m_pending.size() > m_maxPendingBytes) {
        const qsizetype excess = m_pending.size() - m_maxPendingBytes;
        m_pending.remove(0, excess);
        m_droppedBytes += excess;
        m_levelSkip = 0; // the dropped run may have ended mid-sample; resync on the next write
        qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "Dropped" << excess << "stale bytes to keep the delay bounded (total" << m_droppedBytes << ")";
    }
    drainPending();
}

void MicrophonePlugin::drainPending()
{
    if (!m_output || m_pending.isEmpty()) {
        return;
    }
    if (!m_primed) {
        if (m_pending.size() < m_primeBytes) {
            return;
        }
        m_primed = true;
    }
    const qsizetype toWrite = qMin(m_pending.size(), m_output->freeBytes());
    if (toWrite <= 0) {
        return;
    }
    const qint64 written = m_output->write(m_pending.constData(), toWrite);
    if (written > 0) {
        scanLevels(m_pending.constData(), written);
        m_pending.remove(0, written);
    }
}

void MicrophonePlugin::scanLevels(const char *data, qsizetype size)
{
    // Samples are 16-bit little-endian. A previous write may have ended half way through a sample,
    // in which case m_levelSkip bytes at the start of this chunk belong to that sample.
    qsizetype pos = m_levelSkip;
    for (; pos + 2 <= size; pos += 2) {
        const qint16 sample = qFromLittleEndian<qint16>(data + pos);
        const double value = std::abs(static_cast<double>(sample)) / 32768.0;
        m_levelPeak = qMax(m_levelPeak, value);
        m_levelSquares += value * value;
        ++m_levelSamples;
    }
    m_levelSkip = size - pos; // 0, or 1 when the chunk ended on the first byte of a sample

    if (m_levelTimer.elapsed() < LEVEL_INTERVAL_MS) {
        return;
    }
    const double rms = m_levelSamples > 0 ? std::sqrt(m_levelSquares / static_cast<double>(m_levelSamples)) : 0.0;
    if (++m_levelTicks % 20 == 0 && m_output && m_bytesPerSecond > 0) { // once a second: how much audio sits between us and the device
        qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "Delay budget: output holds" << m_output->queuedMicros() / 1000 << "ms,"
                                              << "pending" << m_pending.size() * 1000 / m_bytesPerSecond << "ms, dropped" << m_droppedBytes << "bytes here,"
                                              << m_output->droppedBytes() << "in the output, underruns" << m_output->underruns();
    }
    Q_EMIT levelChanged(qMin(m_levelPeak, 1.0), qMin(rms, 1.0));
    m_levelPeak = 0.0;
    m_levelSquares = 0.0;
    m_levelSamples = 0;
    m_levelTimer.start();
}

void MicrophonePlugin::stopStream()
{
    if (!m_socket && !m_output) {
        return;
    }

    m_drainTimer.stop();

    if (m_output) {
        m_output->stop();
        m_output.reset();
    }

    if (m_socket) {
        QSharedPointer<QIODevice> socket = m_socket;
        m_socket.reset();
        disconnect(socket.data(), nullptr, this, nullptr);
        socket->close();
        // stopStream may be running inside one of the socket's own signals, so release the last
        // reference (and thereby delete the socket) only once we are back in the event loop.
        QTimer::singleShot(0, this, [socket]() {
            Q_UNUSED(socket);
        });
    }

    m_pending.clear();
    m_primed = false;
    m_outputDevice.clear();
    m_outputMode.clear();
    m_sampleRate = 0;
    m_bytesPerSecond = 0;
    m_startedAt = 0;
    m_levelTimer.invalidate();
    m_levelSkip = 0;
    m_levelPeak = 0.0;
    m_levelSquares = 0.0;
    m_levelSamples = 0;

    qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "Stopped stream" << m_streamId;
    m_streamId.clear();
    Q_EMIT levelChanged(0.0, 0.0);
    Q_EMIT streamingChanged(false);
}

bool MicrophonePlugin::isStreaming() const
{
    return m_output != nullptr;
}

QString MicrophonePlugin::outputDevice() const
{
    return m_outputDevice;
}

int MicrophonePlugin::sampleRate() const
{
    return m_sampleRate;
}

qlonglong MicrophonePlugin::startedAt() const
{
    return m_startedAt;
}

QString MicrophonePlugin::outputMode() const
{
    return m_outputMode;
}

bool MicrophonePlugin::exclusiveMode() const
{
    return config()->getBool(CONFIG_EXCLUSIVE_MODE, true);
}

void MicrophonePlugin::setExclusiveMode(bool exclusive)
{
    if (exclusive == exclusiveMode()) {
        return;
    }
    config()->set(CONFIG_EXCLUSIVE_MODE, exclusive);
    Q_EMIT exclusiveModeChanged(exclusive);
}

void MicrophonePlugin::stop()
{
    stopStream();
}

QString MicrophonePlugin::dbusPath() const
{
    return QStringLiteral("/modules/kdeconnect/devices/%1/microphone").arg(device()->id());
}

#include "microphoneplugin.moc"
#include "moc_microphoneplugin.cpp"
