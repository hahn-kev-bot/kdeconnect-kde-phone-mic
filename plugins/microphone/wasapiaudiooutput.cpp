/**
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "wasapiaudiooutput.h"

#include <QDebug>
#include <QMutexLocker>
#include <QThread>

#include <cstring>

#include <Functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include "plugin_microphone_debug.h"

// REFERENCE_TIME is in 100 ns units.
static const REFERENCE_TIME REFTIMES_PER_SEC = 10000000;
static const DWORD RENDER_WAIT_MS = 100;
// KSDATAFORMAT_SUBTYPE_PCM and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT from ksmedia.h, without that header.
static const GUID SUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

static QString hrString(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

static QString deviceName(IMMDevice *device)
{
    IPropertyStore *properties = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
        return QString();
    }
    QString name;
    PROPVARIANT value = {};
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = QString::fromWCharArray(value.pwszVal);
    }
    PropVariantClear(&value);
    properties->Release();
    return name;
}

static REFERENCE_TIME framesToDuration(DWORD sampleRate, UINT32 frames)
{
    // The rounding Microsoft's AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED recipe uses.
    return static_cast<REFERENCE_TIME>((10000.0 * 1000 / sampleRate * frames) + 0.5);
}

// Exclusive-mode drivers (VB-CABLE included) expect WAVE_FORMAT_EXTENSIBLE; plain WAVE_FORMAT_PCM
// gets AUDCLNT_E_UNSUPPORTED_FORMAT even for formats the device happily plays.
static void fillPcm16(WAVEFORMATEXTENSIBLE *format, int channels, int sampleRate)
{
    *format = {};
    WAVEFORMATEX &base = format->Format;
    base.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    base.nChannels = static_cast<WORD>(channels);
    base.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    base.wBitsPerSample = 16;
    base.nBlockAlign = static_cast<WORD>(channels * 2);
    base.nAvgBytesPerSec = base.nSamplesPerSec * base.nBlockAlign;
    base.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format->Samples.wValidBitsPerSample = 16;
    format->dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    format->SubFormat = SUBTYPE_PCM;
}

/**
 * Classifies a format the device reports as 16-bit PCM (isFloat false), 32-bit float (true), or
 * something this renderer cannot produce (returns false).
 */
static bool classifyFormat(const WAVEFORMATEX *format, bool *isFloat)
{
    if (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) {
        *isFloat = false;
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        *isFloat = true;
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        if (IsEqualGUID(extensible->SubFormat, SUBTYPE_PCM) && format->wBitsPerSample == 16 && extensible->Samples.wValidBitsPerSample == 16) {
            *isFloat = false;
            return true;
        }
        if (IsEqualGUID(extensible->SubFormat, SUBTYPE_IEEE_FLOAT) && format->wBitsPerSample == 32) {
            *isFloat = true;
            return true;
        }
    }
    return false;
}

WasapiAudioOutput::WasapiAudioOutput(int maxQueuedMs)
    : m_maxQueuedMs(maxQueuedMs)
{
}

void WasapiAudioOutput::setExclusive(bool exclusive)
{
    m_exclusiveRequested = exclusive;
}

QString WasapiAudioOutput::mode() const
{
    return m_exclusive ? QStringLiteral("exclusive") : QStringLiteral("shared");
}

WasapiAudioOutput::~WasapiAudioOutput()
{
    stop();
}

bool WasapiAudioOutput::start(int sampleRate, int channels, const QString &deviceSubstring, QString *chosenDescription)
{
    stop();

    // The daemon's main thread already has an apartment (QApplication); S_FALSE just adds a reference.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(coInit);
    if (FAILED(coInit) && coInit != RPC_E_CHANGED_MODE) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: CoInitializeEx failed" << hrString(coInit);
        return false;
    }

    HRESULT hr =
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&m_enumerator));
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: could not create MMDeviceEnumerator" << hrString(hr);
        release();
        return false;
    }

    if (!selectDevice(deviceSubstring, chosenDescription) || !initializeClient(sampleRate, channels)) {
        release();
        return false;
    }

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_frameBytes = static_cast<size_t>(channels) * 2;
    m_lastPadding = 0;
    m_droppedBytes = 0;
    m_underruns = 0;
    m_renderErrors = 0;
    m_ringHead = 0;
    m_ringSize = 0;

    m_thread = QThread::create([this]() {
        renderLoop();
    });
    m_thread->start(QThread::TimeCriticalPriority);
    return true;
}

bool WasapiAudioOutput::selectDevice(const QString &deviceSubstring, QString *chosenDescription)
{
    IMMDeviceCollection *devices = nullptr;
    HRESULT hr = m_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: EnumAudioEndpoints failed" << hrString(hr);
        return false;
    }

    UINT count = 0;
    devices->GetCount(&count);
    QString name;
    for (UINT i = 0; i < count && !m_device; ++i) {
        IMMDevice *candidate = nullptr;
        if (FAILED(devices->Item(i, &candidate))) {
            continue;
        }
        const QString candidateName = deviceName(candidate);
        if (candidateName.contains(deviceSubstring, Qt::CaseInsensitive)) {
            m_device = candidate;
            name = candidateName;
        } else {
            candidate->Release();
        }
    }
    devices->Release();

    if (!m_device) {
        hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        if (FAILED(hr)) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: no audio output matches" << deviceSubstring << "and GetDefaultAudioEndpoint failed"
                                                    << hrString(hr);
            return false;
        }
        name = deviceName(m_device);
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: no audio output matches" << deviceSubstring << ", using default output" << name;
    }

    if (chosenDescription) {
        *chosenDescription = name;
    }
    return true;
}

bool WasapiAudioOutput::activateClient()
{
    if (m_client) {
        m_client->Release();
        m_client = nullptr;
    }
    const HRESULT hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&m_client));
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: IMMDevice::Activate(IAudioClient) failed" << hrString(hr);
        return false;
    }
    return true;
}

bool WasapiAudioOutput::initializeWithRetry(AUDCLNT_SHAREMODE shareMode,
                                            DWORD flags,
                                            REFERENCE_TIME duration,
                                            REFERENCE_TIME period,
                                            const WAVEFORMATEX *format,
                                            HRESULT *result)
{
    HRESULT hr = m_client->Initialize(shareMode, flags, duration, period, format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Exclusive event-driven streams need a buffer the hardware can align; the client tells us
        // the next aligned size, and a failed Initialize leaves it unusable, so re-activate first.
        UINT32 alignedFrames = 0;
        hr = m_client->GetBufferSize(&alignedFrames);
        if (FAILED(hr)) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetBufferSize after unaligned Initialize failed" << hrString(hr);
            *result = hr;
            return false;
        }
        if (!activateClient()) {
            *result = E_FAIL;
            return false;
        }
        const REFERENCE_TIME alignedDuration = framesToDuration(format->nSamplesPerSec, alignedFrames);
        const REFERENCE_TIME alignedPeriod = shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE ? alignedDuration : period;
        hr = m_client->Initialize(shareMode, flags, alignedDuration, alignedPeriod, format, nullptr);
    }
    *result = hr;
    return SUCCEEDED(hr);
}

bool WasapiAudioOutput::initializeExclusive(int sampleRate, int channels, REFERENCE_TIME minimumPeriod)
{
    std::vector<FormatCandidate> candidates;

    FormatCandidate ours;
    fillPcm16(&ours.format, channels, sampleRate);
    ours.channels = channels;
    candidates.push_back(ours);

    if (channels != 2) {
        FormatCandidate stereo;
        fillPcm16(&stereo.format, 2, sampleRate);
        stereo.channels = 2;
        candidates.push_back(stereo);
    }

    WAVEFORMATEX *mixFormat = nullptr;
    if (SUCCEEDED(m_client->GetMixFormat(&mixFormat)) && mixFormat) {
        FormatCandidate mix;
        const size_t mixSize = sizeof(WAVEFORMATEX) + mixFormat->cbSize;
        // No conversion happens in exclusive mode, so the mix format is only usable at our rate.
        if (mixSize <= sizeof(WAVEFORMATEXTENSIBLE) && classifyFormat(mixFormat, &mix.isFloat) && mixFormat->nSamplesPerSec == static_cast<DWORD>(sampleRate)
            && mixFormat->nChannels > 0) {
            std::memcpy(&mix.format, mixFormat, mixSize);
            mix.channels = mixFormat->nChannels;
            candidates.push_back(mix);
        } else {
            qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: mix format not usable for exclusive mode (tag" << mixFormat->wFormatTag << ","
                                                  << mixFormat->wBitsPerSample << "bit," << mixFormat->nSamplesPerSec << "Hz," << mixFormat->nChannels
                                                  << "channels)";
        }
        CoTaskMemFree(mixFormat);
    }

    HRESULT lastError = AUDCLNT_E_UNSUPPORTED_FORMAT;
    for (const FormatCandidate &candidate : candidates) {
        const WAVEFORMATEX *format = &candidate.format.Format;
        // In exclusive mode the closest-match out-param must be null; the answer is S_OK or an error.
        HRESULT hr = m_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, format, nullptr);
        if (hr != S_OK) {
            qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: exclusive format rejected" << hrString(hr) << ":" << candidate.channels << "channels,"
                                                  << (candidate.isFloat ? "float32" : "pcm16");
            lastError = hr;
            continue;
        }

        // Event-driven exclusive streams require buffer duration == period; the minimum period is
        // the lowest latency the endpoint offers.
        if (initializeWithRetry(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimumPeriod, minimumPeriod, format, &hr)) {
            m_exclusive = true;
            m_deviceChannels = candidate.channels;
            m_deviceFloat = candidate.isFloat;
            m_deviceFrameBytes = static_cast<size_t>(candidate.channels) * (candidate.isFloat ? 4 : 2);
            return true;
        }
        qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: exclusive Initialize failed" << hrString(hr) << "for" << candidate.channels << "channels,"
                                              << (candidate.isFloat ? "float32" : "pcm16");
        lastError = hr;
        if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
            break; // another format will not help
        }
        if (!activateClient()) {
            return false;
        }
    }

    qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: exclusive mode unavailable" << hrString(lastError) << ", falling back to shared mode";
    return false;
}

bool WasapiAudioOutput::initializeShared(int sampleRate, int channels)
{
    WAVEFORMATEXTENSIBLE format;
    fillPcm16(&format, channels, sampleRate);

    // AUTOCONVERTPCM lets the engine resample/remix from our format to its mix format, so we never
    // need GetMixFormat. Shared-mode event-driven streams must pass 0 for both durations; the engine
    // then sizes the buffer to its own period, which is the lowest latency shared mode offers.
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT hr = S_OK;
    if (!initializeWithRetry(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format.Format, &hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: IAudioClient::Initialize failed" << hrString(hr) << "for" << sampleRate << "Hz," << channels
                                                << "channels";
        return false;
    }
    m_exclusive = false;
    m_deviceChannels = channels;
    m_deviceFloat = false;
    m_deviceFrameBytes = static_cast<size_t>(channels) * 2;
    return true;
}

bool WasapiAudioOutput::initializeClient(int sampleRate, int channels)
{
    if (!activateClient()) {
        return false;
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    HRESULT hr = m_client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetDevicePeriod failed" << hrString(hr);
        return false;
    }

    m_exclusive = false;
    const bool initialized = m_exclusiveRequested && initializeExclusive(sampleRate, channels, minimumPeriod);
    if (!initialized) {
        // A client whose Initialize failed cannot be initialized again; start from a fresh one.
        if (!activateClient() || !initializeShared(sampleRate, channels)) {
            return false;
        }
    }

    hr = m_client->GetBufferSize(&m_bufferFrames);
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetBufferSize failed" << hrString(hr);
        return false;
    }

    m_bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_bufferEvent || !m_stopEvent) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: CreateEvent failed" << GetLastError();
        return false;
    }

    hr = m_client->SetEventHandle(m_bufferEvent);
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: SetEventHandle failed" << hrString(hr);
        return false;
    }

    hr = m_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&m_renderClient));
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetService(IAudioRenderClient) failed" << hrString(hr);
        return false;
    }

    // Room for the plugin's queue cap plus one device period, so a write that lands just before the
    // engine's wakeup is not thrown away. The ring always holds the stream's own format.
    const qint64 periodFrames = (defaultPeriod * sampleRate + REFTIMES_PER_SEC - 1) / REFTIMES_PER_SEC;
    const qint64 capFrames = static_cast<qint64>(m_maxQueuedMs) * sampleRate / 1000;
    m_ring.assign(static_cast<size_t>((capFrames + periodFrames) * channels * 2), 0);

    qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: endpoint buffer" << m_bufferFrames << "frames, device period" << defaultPeriod / 10000.0 << "ms (min"
                                          << minimumPeriod / 10000.0 << "ms), ring" << (capFrames + periodFrames) << "frames, mode" << mode() << "with"
                                          << m_deviceChannels << "channels" << (m_deviceFloat ? "float32" : "pcm16");
    return true;
}

void WasapiAudioOutput::stop()
{
    if (m_thread) {
        SetEvent(m_stopEvent);
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    if (m_client && m_clientStarted) {
        m_client->Stop();
    }
    m_clientStarted = false;
    release();
}

void WasapiAudioOutput::release()
{
    if (m_renderClient) {
        m_renderClient->Release();
        m_renderClient = nullptr;
    }
    if (m_client) {
        m_client->Release();
        m_client = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    if (m_enumerator) {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
    if (m_bufferEvent) {
        CloseHandle(m_bufferEvent);
        m_bufferEvent = nullptr;
    }
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
    m_ring.clear();
    m_ringHead = 0;
    m_ringSize = 0;
    m_bufferFrames = 0;
    m_exclusive = false;
}

qint64 WasapiAudioOutput::write(const char *data, qsizetype bytes)
{
    if (!m_renderClient || bytes <= 0) {
        return 0;
    }

    {
        QMutexLocker locker(&m_mutex);
        const size_t capacity = m_ring.size();
        size_t toWrite = static_cast<size_t>(bytes);
        if (toWrite > capacity) {
            // Only the tail of an oversized chunk can ever survive.
            m_droppedBytes += static_cast<qint64>(toWrite - capacity);
            data += toWrite - capacity;
            toWrite = capacity;
        }
        const size_t free = capacity - m_ringSize;
        if (toWrite > free) {
            const size_t overflow = toWrite - free;
            m_ringHead = (m_ringHead + overflow) % capacity;
            m_ringSize -= overflow;
            m_droppedBytes += static_cast<qint64>(overflow);
        }
        const size_t tail = (m_ringHead + m_ringSize) % capacity;
        const size_t firstChunk = qMin(toWrite, capacity - tail);
        std::memcpy(m_ring.data() + tail, data, firstChunk);
        if (firstChunk < toWrite) {
            std::memcpy(m_ring.data(), data + firstChunk, toWrite - firstChunk);
        }
        m_ringSize += toWrite;
    }

    if (!m_clientStarted) {
        // The caller has primed us; fill the endpoint buffer before the engine starts pulling so
        // the first period is not silence. No events fire before Start, so the render thread is idle.
        fillDevice(false);
        const HRESULT hr = m_client->Start();
        if (FAILED(hr)) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: IAudioClient::Start failed" << hrString(hr);
        }
        m_clientStarted = true;
    }
    return bytes;
}

void WasapiAudioOutput::convertFrames(const char *source, BYTE *destination, size_t frames) const
{
    // Source frames are interleaved 16-bit little-endian with m_channels channels. Device channels
    // beyond the source count repeat the source channels (mono becomes dual mono); extra source
    // channels are dropped.
    const auto *samples = reinterpret_cast<const qint16 *>(source);
    if (m_deviceFloat) {
        auto *out = reinterpret_cast<float *>(destination);
        for (size_t frame = 0; frame < frames; ++frame) {
            const qint16 *in = samples + frame * m_channels;
            for (int channel = 0; channel < m_deviceChannels; ++channel) {
                *out++ = static_cast<float>(in[channel % m_channels]) / 32768.0f;
            }
        }
    } else {
        auto *out = reinterpret_cast<qint16 *>(destination);
        for (size_t frame = 0; frame < frames; ++frame) {
            const qint16 *in = samples + frame * m_channels;
            for (int channel = 0; channel < m_deviceChannels; ++channel) {
                *out++ = in[channel % m_channels];
            }
        }
    }
}

bool WasapiAudioOutput::fillDevice(bool countUnderrun)
{
    UINT32 frames = m_bufferFrames;
    if (m_exclusive) {
        // Event-driven exclusive streams ping-pong two buffers: each event hands us a whole
        // buffer, GetBuffer must ask for exactly that size, and GetCurrentPadding is meaningless.
        m_lastPadding = 0;
    } else {
        UINT32 padding = 0;
        const HRESULT hr = m_client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            if (m_renderErrors++ == 0) {
                qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetCurrentPadding failed" << hrString(hr);
            }
            return false;
        }
        m_lastPadding = padding;
        if (padding >= m_bufferFrames) {
            return true;
        }
        frames = m_bufferFrames - padding;
    }

    BYTE *buffer = nullptr;
    HRESULT hr = m_renderClient->GetBuffer(frames, &buffer);
    if (FAILED(hr)) {
        if (m_renderErrors++ == 0) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetBuffer failed" << hrString(hr);
        }
        return false;
    }

    const bool sameFormat = m_deviceFrameBytes == m_frameBytes && !m_deviceFloat;
    const size_t wanted = static_cast<size_t>(frames) * m_frameBytes;
    size_t copied = 0;
    {
        QMutexLocker locker(&m_mutex);
        const size_t capacity = m_ring.size();
        copied = qMin(wanted, m_ringSize - (m_ringSize % m_frameBytes));
        // The ring may wrap inside what we take; neither chunk splits a frame because the
        // capacity, m_ringHead and copied are all whole frames.
        const size_t firstChunk = qMin(copied, capacity - m_ringHead);
        if (sameFormat) {
            std::memcpy(buffer, m_ring.data() + m_ringHead, firstChunk);
            if (firstChunk < copied) {
                std::memcpy(buffer + firstChunk, m_ring.data(), copied - firstChunk);
            }
        } else {
            const size_t firstFrames = firstChunk / m_frameBytes;
            convertFrames(m_ring.data() + m_ringHead, buffer, firstFrames);
            if (firstChunk < copied) {
                convertFrames(m_ring.data(), buffer + firstFrames * m_deviceFrameBytes, (copied - firstChunk) / m_frameBytes);
            }
        }
        m_ringHead = (m_ringHead + copied) % capacity;
        m_ringSize -= copied;
    }

    const size_t copiedFrames = copied / m_frameBytes;
    DWORD flags = 0;
    if (copiedFrames == 0) {
        flags = AUDCLNT_BUFFERFLAGS_SILENT;
    } else if (copiedFrames < frames) {
        // Zero is silence for both 16-bit PCM and float.
        std::memset(buffer + copiedFrames * m_deviceFrameBytes, 0, (frames - copiedFrames) * m_deviceFrameBytes);
    }
    if (copiedFrames < frames && countUnderrun) {
        ++m_underruns;
    }

    hr = m_renderClient->ReleaseBuffer(frames, flags);
    if (FAILED(hr)) {
        if (m_renderErrors++ == 0) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: ReleaseBuffer failed" << hrString(hr);
        }
        return false;
    }
    m_lastPadding = m_bufferFrames;
    return true;
}

void WasapiAudioOutput::renderLoop()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!task) {
        qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: AvSetMmThreadCharacteristics failed" << GetLastError() << ", rendering at normal priority";
    }

    const HANDLE handles[2] = {m_stopEvent, m_bufferEvent};
    for (;;) {
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, RENDER_WAIT_MS);
        if (waited == WAIT_OBJECT_0) {
            break;
        }
        if (waited == WAIT_OBJECT_0 + 1) {
            fillDevice(true);
        } else if (waited == WAIT_FAILED) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: WaitForMultipleObjects failed" << GetLastError();
            break;
        }
        // WAIT_TIMEOUT: the engine has not started yet, or is stalled; keep waiting.
    }

    if (task) {
        AvRevertMmThreadCharacteristics(task);
    }
    if (SUCCEEDED(coInit)) {
        CoUninitialize();
    }
}

qint64 WasapiAudioOutput::queuedMicros() const
{
    if (!m_client || m_sampleRate <= 0) {
        return 0;
    }
    qint64 ringFrames = 0;
    {
        QMutexLocker locker(&m_mutex);
        ringFrames = static_cast<qint64>(m_ringSize / m_frameBytes);
    }
    return (ringFrames + m_lastPadding.load()) * 1000000 / m_sampleRate;
}

qsizetype WasapiAudioOutput::freeBytes() const
{
    if (!m_client) {
        return 0;
    }
    QMutexLocker locker(&m_mutex);
    return static_cast<qsizetype>(m_ring.size() - m_ringSize);
}

qint64 WasapiAudioOutput::droppedBytes() const
{
    return m_droppedBytes.load();
}

qint64 WasapiAudioOutput::underruns() const
{
    return m_underruns.load();
}
