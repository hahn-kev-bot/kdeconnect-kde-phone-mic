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

WasapiAudioOutput::WasapiAudioOutput(int maxQueuedMs)
    : m_maxQueuedMs(maxQueuedMs)
{
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

bool WasapiAudioOutput::initializeClient(int sampleRate, int channels)
{
    HRESULT hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&m_client));
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: IMMDevice::Activate(IAudioClient) failed" << hrString(hr);
        return false;
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    hr = m_client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetDevicePeriod failed" << hrString(hr);
        return false;
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(channels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    // AUTOCONVERTPCM lets the engine resample/remix from our format to its mix format, so we never
    // need GetMixFormat. Shared-mode event-driven streams must pass 0 for both durations; the engine
    // then sizes the buffer to its own period, which is the lowest latency shared mode offers.
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Documented for exclusive mode only, but harmless to honour: retry with the aligned size.
        UINT32 alignedFrames = 0;
        hr = m_client->GetBufferSize(&alignedFrames);
        if (FAILED(hr)) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetBufferSize after unaligned Initialize failed" << hrString(hr);
            return false;
        }
        m_client->Release();
        m_client = nullptr;
        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&m_client));
        if (FAILED(hr)) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: IMMDevice::Activate(IAudioClient) retry failed" << hrString(hr);
            return false;
        }
        const REFERENCE_TIME alignedDuration = static_cast<REFERENCE_TIME>((10000.0 * 1000 / format.nSamplesPerSec * alignedFrames) + 0.5);
        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, alignedDuration, 0, &format, nullptr);
    }
    if (FAILED(hr)) {
        qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: IAudioClient::Initialize failed" << hrString(hr) << "for" << sampleRate << "Hz," << channels
                                                << "channels";
        return false;
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
    // engine's wakeup is not thrown away.
    const qint64 periodFrames = (defaultPeriod * sampleRate + REFTIMES_PER_SEC - 1) / REFTIMES_PER_SEC;
    const qint64 capFrames = static_cast<qint64>(m_maxQueuedMs) * sampleRate / 1000;
    m_ring.assign(static_cast<size_t>((capFrames + periodFrames) * channels * 2), 0);

    qCDebug(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: endpoint buffer" << m_bufferFrames << "frames, device period" << defaultPeriod / 10000.0 << "ms, ring"
                                          << (capFrames + periodFrames) << "frames";
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

bool WasapiAudioOutput::fillDevice(bool countUnderrun)
{
    UINT32 padding = 0;
    HRESULT hr = m_client->GetCurrentPadding(&padding);
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
    const UINT32 frames = m_bufferFrames - padding;

    BYTE *buffer = nullptr;
    hr = m_renderClient->GetBuffer(frames, &buffer);
    if (FAILED(hr)) {
        if (m_renderErrors++ == 0) {
            qCWarning(KDECONNECT_PLUGIN_MICROPHONE) << "WASAPI: GetBuffer failed" << hrString(hr);
        }
        return false;
    }

    const size_t wanted = static_cast<size_t>(frames) * m_frameBytes;
    size_t copied = 0;
    {
        QMutexLocker locker(&m_mutex);
        const size_t capacity = m_ring.size();
        copied = qMin(wanted, m_ringSize - (m_ringSize % m_frameBytes));
        const size_t firstChunk = qMin(copied, capacity - m_ringHead);
        std::memcpy(buffer, m_ring.data() + m_ringHead, firstChunk);
        if (firstChunk < copied) {
            std::memcpy(buffer + firstChunk, m_ring.data(), copied - firstChunk);
        }
        m_ringHead = (m_ringHead + copied) % capacity;
        m_ringSize -= copied;
    }

    DWORD flags = 0;
    if (copied == 0) {
        flags = AUDCLNT_BUFFERFLAGS_SILENT;
    } else if (copied < wanted) {
        std::memset(buffer + copied, 0, wanted - copied);
    }
    if (copied < wanted && countUnderrun) {
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
