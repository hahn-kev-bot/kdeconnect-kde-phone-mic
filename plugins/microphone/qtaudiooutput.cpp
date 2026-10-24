/**
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "qtaudiooutput.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include "plugin_microphone_debug.h"

// Microseconds of audio the sink keeps buffered.
static const qint64 SINK_BUFFER_DURATION = 40000;

QtAudioOutput::~QtAudioOutput()
{
    stop();
}

bool QtAudioOutput::start(int sampleRate, int channels, const QString &deviceSubstring, QString *chosenDescription)
{
    stop();

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!format.isValid()) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "Invalid audio format" << sampleRate << "Hz," << channels << "channels";
        return false;
    }

    QAudioDevice device;
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    for (const QAudioDevice &candidate : outputs) {
        if (candidate.description().contains(deviceSubstring, Qt::CaseInsensitive)) {
            device = candidate;
            break;
        }
    }
    if (device.isNull()) {
        device = QMediaDevices::defaultAudioOutput();
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "No audio output matches" << deviceSubstring << ", using default output" << device.description();
    }
    if (device.isNull()) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "No audio output device available";
        return false;
    }
    if (!device.isFormatSupported(format)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "Audio output" << device.description() << "does not support" << sampleRate << "Hz," << channels
                                                << "channels, Int16";
        return false;
    }

    m_format = format;
    m_sink = std::make_unique<QAudioSink>(device, format);
    m_sink->setBufferSize(format.bytesForDuration(SINK_BUFFER_DURATION));
    m_sinkDevice = m_sink->start();
    if (!m_sinkDevice) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "Could not start audio output" << device.description() << "error" << static_cast<int>(m_sink->error());
        stop();
        return false;
    }

    if (chosenDescription) {
        *chosenDescription = device.description();
    }
    return true;
}

void QtAudioOutput::stop()
{
    if (m_sink) {
        m_sink->stop();
        m_sink.reset();
    }
    m_sinkDevice = nullptr;
}

qint64 QtAudioOutput::write(const char *data, qsizetype bytes)
{
    if (!m_sinkDevice) {
        return 0;
    }
    const qint64 written = m_sinkDevice->write(data, qMin(bytes, m_sink->bytesFree()));
    return written > 0 ? written : 0;
}

qint64 QtAudioOutput::queuedMicros() const
{
    if (!m_sink) {
        return 0;
    }
    return m_format.durationForBytes(static_cast<qint32>(m_sink->bufferSize() - m_sink->bytesFree()));
}

qsizetype QtAudioOutput::freeBytes() const
{
    return m_sink ? m_sink->bytesFree() : 0;
}
