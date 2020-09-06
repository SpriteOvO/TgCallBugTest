#include <iostream>

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#define TASSERT(condition)    while(!(condition)) { __debugbreak(); }
#define LOG(content)          std::wcout << content << std::endl


template <class T>
void SafeRelease(T * *ppT)
{
    if (*ppT) {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

class AudioOutput : public IMMNotificationClient, IAudioSessionEvents
{
public:
    AudioOutput()
    {
        _IsPlaying = FALSE;
        _RemainingDataLen = 0;

        HRESULT Result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        TASSERT(SUCCEEDED(Result));

        _hShutdownEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
        _hAudioSamplesReadyEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
        _hStreamSwitchEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);

        ZeroMemory(&_Format, sizeof(_Format));
        _Format.wFormatTag = WAVE_FORMAT_PCM;
        _Format.nChannels = 1;
        _Format.nSamplesPerSec = 48000;
        _Format.nBlockAlign = 2;
        _Format.nAvgBytesPerSec = _Format.nSamplesPerSec * _Format.nBlockAlign;
        _Format.wBitsPerSample = 16;

        Result = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_pEnumerator));
        TASSERT(SUCCEEDED(Result));

        Result = _pEnumerator->RegisterEndpointNotificationCallback(this);
        TASSERT(SUCCEEDED(Result));

        _pAudioSessionControl = NULL;
        _pDevice = NULL;
        _pAudioClient = NULL;
        _pRenderClient = NULL;
        _hThread = NULL;
    }

    ~AudioOutput()
    {
        if (_pAudioClient != NULL) {
            _pAudioClient->Stop();
        }
        if (_pAudioSessionControl != NULL) {
            _pAudioSessionControl->UnregisterAudioSessionNotification(this);
        }

        SetEvent(_hShutdownEvent);

        if (_hThread != NULL) {
            WaitForSingleObjectEx(_hThread, INFINITE, FALSE);
            CloseHandle(_hThread);
        }

        SafeRelease(&_pAudioSessionControl);
        SafeRelease(&_pRenderClient);
        SafeRelease(&_pAudioClient);
        SafeRelease(&_pDevice);

        CloseHandle(_hShutdownEvent);
        CloseHandle(_hAudioSamplesReadyEvent);
        CloseHandle(_hStreamSwitchEvent);

        if (_pEnumerator != NULL) {
            _pEnumerator->UnregisterEndpointNotificationCallback(this);
            SafeRelease(&_pEnumerator);
        }
    }

    void Start()
    {
        _IsPlaying = TRUE;
        if (_hThread == NULL) {
            _hThread = CreateThread(NULL, 0, &AudioOutput::StartThread, this, 0, NULL);
        }
    }

    void Stop()
    {
        _IsPlaying = FALSE;
    }

    void RunThread()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        HANDLE WaitArray[] = { _hShutdownEvent, _hStreamSwitchEvent, _hAudioSamplesReadyEvent };
        HRESULT Result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        TASSERT(SUCCEEDED(Result));

        UINT32 BufferSize;
        UINT64 FramesWritten = 0;

        Result = _pAudioClient->GetBufferSize(&BufferSize);
        TASSERT(SUCCEEDED(Result));

        BOOLEAN Running = TRUE;

        while (Running)
        {
            DWORD WaitResult = WaitForMultipleObjectsEx(3, WaitArray, FALSE, INFINITE, FALSE);

            if (WaitResult == WAIT_OBJECT_0) { // ShutdownEvent
                Running = FALSE;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1) { // streamSwitchEvent
                ActuallySetCurrentDevice(_StreamChangeToDevice);
                ResetEvent(_hStreamSwitchEvent);
            }
            else if (WaitResult == WAIT_OBJECT_0 + 2) // audioSamplesReadyEvent
            {
                if (!_pAudioClient) {
                    continue;
                }

                BYTE *pData;
                UINT32 Padding;
                UINT32 FramesAvailable;

                Result = _pAudioClient->GetCurrentPadding(&Padding);
                TASSERT(SUCCEEDED(Result));

                FramesAvailable = BufferSize - Padding;
                Result = _pRenderClient->GetBuffer(FramesAvailable, &pData);
                TASSERT(SUCCEEDED(Result));

                SIZE_T BytesAvailable = FramesAvailable * 2;
                while (BytesAvailable > _RemainingDataLen) {
                    RtlZeroMemory(_RemainingData + _RemainingDataLen, 960 * 2);
                    _RemainingDataLen += 960 * 2;
                }

                memcpy(pData, _RemainingData, BytesAvailable);
                if (_RemainingDataLen > BytesAvailable) {
                    RtlMoveMemory(_RemainingData, _RemainingData + BytesAvailable, _RemainingDataLen - BytesAvailable);
                }
                _RemainingDataLen -= BytesAvailable;

                Result = _pRenderClient->ReleaseBuffer(FramesAvailable, 0);
                TASSERT(SUCCEEDED(Result));
                FramesWritten += FramesAvailable;
            }
        }
    }

    void SetCurrentDevice(const std::wstring &DeviceId)
    {
        if (_hThread != NULL) {
            _StreamChangeToDevice = DeviceId;
            SetEvent(_hStreamSwitchEvent);
        }
        else {
            ActuallySetCurrentDevice(DeviceId);
        }
    }

    void ActuallySetCurrentDevice(const std::wstring &DeviceId)
    {
        _CurrentDevice = DeviceId;

        HRESULT Result;

        if (_pAudioClient != NULL) {
            Result = _pAudioClient->Stop();
            TASSERT(SUCCEEDED(Result));
        }
        if (_pAudioSessionControl != NULL) {
            Result = _pAudioSessionControl->UnregisterAudioSessionNotification(this);
            TASSERT(SUCCEEDED(Result));
        }

        SafeRelease(&_pAudioSessionControl);
        SafeRelease(&_pRenderClient);
        SafeRelease(&_pAudioClient);
        SafeRelease(&_pDevice);

        if (DeviceId == L"default")
        {
            _IsDefaultDevice = TRUE;

            IMMDevice *pDefaultDevice;
            Result = _pEnumerator->GetDefaultAudioEndpoint(eRender, eCommunications, &pDefaultDevice);
            TASSERT(SUCCEEDED(Result));

            WCHAR *pDefaultDeviceId;
            Result = pDefaultDevice->GetId(&pDefaultDeviceId);
            TASSERT(SUCCEEDED(Result));

            Result = _pEnumerator->GetDevice(pDefaultDeviceId, &_pDevice);
            TASSERT(SUCCEEDED(Result));

            CoTaskMemFree(pDefaultDeviceId);
            SafeRelease(&pDefaultDevice);
        }
        else
        {
            _IsDefaultDevice = FALSE;

            IMMDeviceCollection *pDeviceCollection = NULL;
            Result = _pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDeviceCollection);
            TASSERT(SUCCEEDED(Result));

            UINT DevicesCount;
            Result = pDeviceCollection->GetCount(&DevicesCount);
            TASSERT(SUCCEEDED(Result));

            for (UINT i = 0; i < DevicesCount; i++)
            {
                IMMDevice *pDevice;
                Result = pDeviceCollection->Item(i, &pDevice);
                TASSERT(SUCCEEDED(Result));

                WCHAR *pDeviceId;
                Result = pDevice->GetId(&pDeviceId);
                TASSERT(SUCCEEDED(Result));

                BOOLEAN IsSame = DeviceId == pDeviceId;
                CoTaskMemFree(pDeviceId);

                if (IsSame) {
                    _pDevice = pDevice;
                    break;
                }
                else {
                    SafeRelease(&pDevice);
                }
            }

            SafeRelease(&pDeviceCollection);
        }

        TASSERT(_pDevice != NULL);

        Result = _pDevice->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, NULL, (void**)&_pAudioClient);
        TASSERT(SUCCEEDED(Result));

        const GUID Guid = { 0x2c693079, 0x3f59, 0x49fd, { 0x96, 0x4f, 0x61, 0xc0, 0x5, 0xea, 0xa5, 0xd3 } };
        Result = _pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 60 * 10000, 0, &_Format, &Guid);
        TASSERT(SUCCEEDED(Result));

        UINT32 BufferSize;
        Result = _pAudioClient->GetBufferSize(&BufferSize);
        TASSERT(SUCCEEDED(Result));

        Result = _pAudioClient->SetEventHandle(_hAudioSamplesReadyEvent);
        TASSERT(SUCCEEDED(Result));

        Result = _pAudioClient->GetService(IID_PPV_ARGS(&_pRenderClient));
        TASSERT(SUCCEEDED(Result));

        BYTE *pData;
        Result = _pRenderClient->GetBuffer(BufferSize, &pData);
        TASSERT(SUCCEEDED(Result));

        Result = _pRenderClient->ReleaseBuffer(BufferSize, AUDCLNT_BUFFERFLAGS_SILENT);
        TASSERT(SUCCEEDED(Result));

        Result = _pAudioClient->GetService(IID_PPV_ARGS(&_pAudioSessionControl));
        TASSERT(SUCCEEDED(Result));

        Result = _pAudioSessionControl->RegisterAudioSessionNotification(this);
        TASSERT(SUCCEEDED(Result));

        _pAudioClient->Start();
    }


private:
    WAVEFORMATEX _Format;
    IMMDeviceEnumerator *_pEnumerator;
    IAudioSessionControl *_pAudioSessionControl;
    IMMDevice *_pDevice;
    IAudioClient *_pAudioClient = NULL;
    IAudioRenderClient *_pRenderClient = NULL;

    HANDLE _hShutdownEvent;
    HANDLE _hAudioSamplesReadyEvent;
    HANDLE _hStreamSwitchEvent;
    HANDLE _hThread;

    ULONG _RefCount;

    BOOLEAN _IsPlaying;

    BYTE _RemainingData[10240];
    SIZE_T _RemainingDataLen;

    std::wstring _CurrentDevice;
    BOOLEAN _IsDefaultDevice;
    std::wstring _StreamChangeToDevice;

    static DWORD WINAPI StartThread(void* arg) {
        ((AudioOutput*)arg)->RunThread();
        return 0;
    }

    STDMETHOD_(ULONG, AddRef)()
    {
        return InterlockedIncrement(&_RefCount);
    }
    STDMETHOD_(ULONG, Release)()
    {
        return InterlockedDecrement(&_RefCount);
    }
    STDMETHOD(OnDisplayNameChanged)(LPCWSTR /*NewDisplayName*/, LPCGUID /*EventContext*/)
    {
        return S_OK;
    }
    STDMETHOD(OnIconPathChanged)(LPCWSTR /*NewIconPath*/, LPCGUID /*EventContext*/)
    {
        return S_OK;
    }
    STDMETHOD(OnSimpleVolumeChanged)(float /*NewSimpleVolume*/, BOOL /*NewMute*/, LPCGUID /*EventContext*/)
    {
        return S_OK;
    }
    STDMETHOD(OnChannelVolumeChanged)(DWORD /*ChannelCount*/, float /*NewChannelVolumes*/[], DWORD /*ChangedChannel*/, LPCGUID /*EventContext*/)
    {
        return S_OK;
    }
    STDMETHOD(OnGroupingParamChanged)(LPCGUID /*NewGroupingParam*/, LPCGUID /*EventContext*/)
    {
        return S_OK;
    }
    STDMETHOD(OnStateChanged)(AudioSessionState /*NewState*/)
    {
        return S_OK;
    }
    STDMETHOD(OnSessionDisconnected)(AudioSessionDisconnectReason DisconnectReason)
    {
        if (!_IsDefaultDevice) {
            _StreamChangeToDevice = L"default";
            SetEvent(_hStreamSwitchEvent);
        }
        return S_OK;
    }
    STDMETHOD(OnDeviceStateChanged)(LPCWSTR /*DeviceId*/, DWORD /*NewState*/)
    {
        return S_OK;
    }
    STDMETHOD(OnDeviceAdded)(LPCWSTR /*DeviceId*/)
    {
        return S_OK;
    };
    STDMETHOD(OnDeviceRemoved)(LPCWSTR /*DeviceId(*/)
    {
        return S_OK;
    };
    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow Flow, ERole Role, LPCWSTR NewDefaultDeviceId)
    {
        if (Flow == eRender && Role == eCommunications && _IsDefaultDevice) {
            _StreamChangeToDevice = L"default";
            SetEvent(_hStreamSwitchEvent);
        }
        return S_OK;
    }
    STDMETHOD(OnPropertyValueChanged)(LPCWSTR /*DeviceId*/, const PROPERTYKEY /*Key*/)
    {
        return S_OK;
    };
    STDMETHOD(QueryInterface)(REFIID iid, void **pvObject)
    {
        if (!pvObject) {
            return E_POINTER;
        }
        *pvObject = NULL;

        if (iid == IID_IUnknown) {
            *pvObject = static_cast<IUnknown*>(static_cast<IAudioSessionEvents*>(this));
            AddRef();
        }
        else if (iid == __uuidof(IMMNotificationClient)) {
            *pvObject = static_cast<IMMNotificationClient*>(this);
            AddRef();
        }
        else if (iid == __uuidof(IAudioSessionEvents)) {
            *pvObject = static_cast<IAudioSessionEvents*>(this);
            AddRef();
        }
        else {
            return E_NOINTERFACE;
        }

        return S_OK;
    }
};

int main()
{
    AudioOutput Ao{};

    Ao.SetCurrentDevice(L"default");
    //Ao.SetCurrentDevice(L"*** My device id ***");

    Ao.Start();

    while (TRUE) {
        Sleep(1000);
    }
    return 0;
}
