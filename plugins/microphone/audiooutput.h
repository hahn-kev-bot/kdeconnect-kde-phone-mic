/**
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QString>
#include <QtGlobal>

/**
 * A playback device for 16-bit little-endian interleaved PCM. Implementations are driven from the
 * plugin's thread: write() must not block, and the plugin polls freeBytes() to pace itself.
 */
class AudioOutput
{
public:
    virtual ~AudioOutput() = default;

    /**
     * Opens the first output device whose name contains deviceSubstring (case-insensitive), or the
     * default output if none matches. On success chosenDescription receives the device name.
     */
    virtual bool start(int sampleRate, int channels, const QString &deviceSubstring, QString *chosenDescription) = 0;
    virtual void stop() = 0;

    /**
     * Queues audio for playback without blocking. Returns the number of bytes accepted.
     */
    virtual qint64 write(const char *data, qsizetype bytes) = 0;

    /**
     * Microseconds of audio queued but not yet rendered, including whatever the device holds.
     */
    virtual qint64 queuedMicros() const = 0;
    virtual qsizetype freeBytes() const = 0;

    /**
     * Bytes the output itself discarded to keep its queue bounded, and the number of device
     * buffers that had to be padded with silence because the queue ran dry.
     */
    virtual qint64 droppedBytes() const
    {
        return 0;
    }
    virtual qint64 underruns() const
    {
        return 0;
    }

    /**
     * How the device is opened: "exclusive" bypasses the system mixer, "shared" goes through it.
     */
    virtual QString mode() const
    {
        return QStringLiteral("shared");
    }
};
