/**
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include "audiooutput.h"

#include <QAudioFormat>
#include <QAudioSink>

#include <memory>

class QIODevice;

/**
 * Plays through a QAudioSink in push mode with a 40 ms buffer. Used on every platform except
 * Windows, and on Windows when the WASAPI output cannot be opened.
 */
class QtAudioOutput : public AudioOutput
{
public:
    QtAudioOutput() = default;
    ~QtAudioOutput() override;

    bool start(int sampleRate, int channels, const QString &deviceSubstring, QString *chosenDescription) override;
    void stop() override;
    qint64 write(const char *data, qsizetype bytes) override;
    qint64 queuedMicros() const override;
    qsizetype freeBytes() const override;

private:
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice *m_sinkDevice = nullptr;
    QAudioFormat m_format;
};
