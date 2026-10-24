/**
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include "audiooutput.h"

#include <QMutex>

#include <atomic>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

class QThread;

/**
 * Event-driven WASAPI renderer. The audio engine signals an event once per device period
 * (typically 10 ms); a dedicated thread then tops the endpoint buffer up from a small ring buffer
 * that write() feeds. Audio reaches the device after about one endpoint buffer (two periods)
 * instead of the extra buffering QAudioSink adds on Windows.
 *
 * By default the device is opened in exclusive mode, which bypasses the audio engine's mixing
 * path and its ~30-40 ms of extra buffering. Exclusive mode cannot convert formats, so the
 * device is asked for 16-bit PCM at the stream's rate (our channel count, then stereo), or for
 * its own mix format if that is 16-bit PCM or 32-bit float; samples are expanded on the way from
 * the ring to the endpoint buffer. If no exclusive attempt succeeds (format rejected, device in
 * use, exclusive mode disabled by the user), the same start() falls back to shared mode.
 */
class WasapiAudioOutput : public AudioOutput
{
public:
    /**
     * @param maxQueuedMs how much audio write() may hold before it drops the oldest data
     */
    explicit WasapiAudioOutput(int maxQueuedMs);
    ~WasapiAudioOutput() override;

    /**
     * Whether start() should try exclusive mode first. Takes effect at the next start().
     */
    void setExclusive(bool exclusive);
    QString mode() const override;

    bool start(int sampleRate, int channels, const QString &deviceSubstring, QString *chosenDescription) override;
    void stop() override;
    qint64 write(const char *data, qsizetype bytes) override;
    qint64 queuedMicros() const override;
    qsizetype freeBytes() const override;
    qint64 droppedBytes() const override;
    qint64 underruns() const override;

private:
    struct FormatCandidate {
        WAVEFORMATEXTENSIBLE format = {};
        int channels = 0;
        bool isFloat = false;
    };

    bool selectDevice(const QString &deviceSubstring, QString *chosenDescription);
    bool initializeClient(int sampleRate, int channels);
    bool initializeExclusive(int sampleRate, int channels, REFERENCE_TIME minimumPeriod);
    bool initializeShared(int sampleRate, int channels);
    bool initializeWithRetry(AUDCLNT_SHAREMODE shareMode, DWORD flags, REFERENCE_TIME duration, REFERENCE_TIME period, const WAVEFORMATEX *format, HRESULT *result);
    bool activateClient();
    void convertFrames(const char *source, BYTE *destination, size_t frames) const;
    bool fillDevice(bool countUnderrun);
    void renderLoop();
    void release();

    const int m_maxQueuedMs;
    bool m_exclusiveRequested = true;
    bool m_exclusive = false;

    IMMDeviceEnumerator *m_enumerator = nullptr;
    IMMDevice *m_device = nullptr;
    IAudioClient *m_client = nullptr;
    IAudioRenderClient *m_renderClient = nullptr;
    HANDLE m_bufferEvent = nullptr;
    HANDLE m_stopEvent = nullptr;
    QThread *m_thread = nullptr;
    bool m_comInitialized = false;
    bool m_clientStarted = false;

    int m_sampleRate = 0;
    int m_channels = 0;
    size_t m_frameBytes = 0;
    // The endpoint's format: same as the ring in shared mode, possibly wider in exclusive mode.
    int m_deviceChannels = 0;
    bool m_deviceFloat = false;
    size_t m_deviceFrameBytes = 0;
    UINT32 m_bufferFrames = 0;

    // Ring buffer of frames waiting for the render thread, guarded by m_mutex.
    mutable QMutex m_mutex;
    std::vector<char> m_ring;
    size_t m_ringHead = 0;
    size_t m_ringSize = 0;

    std::atomic<UINT32> m_lastPadding{0};
    std::atomic<qint64> m_droppedBytes{0};
    std::atomic<qint64> m_underruns{0};
    std::atomic<int> m_renderErrors{0};
};
