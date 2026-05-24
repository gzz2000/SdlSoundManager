#include "SdlSoundManager.h"

#include <math.h>
#include <string.h>

#ifdef LoadImage
#undef LoadImage
#endif

#ifdef GetClassName
#undef GetClassName
#endif

#define SDL_SOUNDMANAGER_GUID SOUND_MANAGER_GUID

// Helper macro for min/max
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

#define SDL_SOUND_PI        3.14159265358979323846f
#define SDL_SOUND_SPEED     343.3f

//-----------------------------------------------------------------------------
// Plugin Entry Points
//-----------------------------------------------------------------------------

#ifdef CK_LIB
    #define CreateNewManager               CreateNewSoundManager
    #define RemoveManager                  RemoveSoundManager
    #define CKGetPluginInfoCount           CKGet_SoundManager_PluginInfoCount
    #define CKGetPluginInfo                CKGet_SoundManager_PluginInfo
    #define g_PluginInfo                   g_SoundManager_PluginInfo
#endif

// Plugin export info
static CKPluginInfo g_PluginInfo;
static const char *SdlSoundManagerName = "SDL Sound Manager";

static float ClampFloat(float value, float minValue, float maxValue)
{
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static Sint16 ClampSint16(float value)
{
    if (value < -32768.0f) return -32768;
    if (value > 32767.0f) return 32767;
    return (Sint16)value;
}

static Sint32 ClampSint32(double value)
{
    if (value < -2147483648.0) return (Sint32)(-2147483647 - 1);
    if (value > 2147483647.0) return 2147483647;
    return (Sint32)value;
}

static CKBOOL MixMatrixChanged(float oldLL, float oldLR, float oldRL, float oldRR,
                               float newLL, float newLR, float newRL, float newRR)
{
    return fabsf(oldLL - newLL) > 0.01f ||
           fabsf(oldLR - newLR) > 0.01f ||
           fabsf(oldRL - newRL) > 0.01f ||
           fabsf(oldRR - newRR) > 0.01f;
}

static int GetSampleBytes(const CKWaveFormat &format)
{
    if (format.wBitsPerSample == 0) return 0;
    return (int)(format.wBitsPerSample / 8);
}

static int GetStreamBlockAlign(const CKWaveFormat &format)
{
    int sampleBytes = GetSampleBytes(format);
    int channels = (format.nChannels == 1) ? 2 : format.nChannels;

    if (sampleBytes <= 0 || channels <= 0) return 0;
    return sampleBytes * channels;
}

static int StreamBytesToSourceBytes(int streamBytes, const CKWaveFormat &format, float frameScale)
{
    int streamBlockAlign = GetStreamBlockAlign(format);
    double sourceBytes;

    if (streamBytes <= 0) return 0;
    if (streamBlockAlign <= 0 || format.nBlockAlign == 0) return streamBytes;

    sourceBytes = ((double)streamBytes / (double)streamBlockAlign) *
                  (double)format.nBlockAlign * (double)frameScale;
    if (sourceBytes <= 0.0) return 0;
    if (sourceBytes > 2147483647.0) return 2147483647;
    return (int)ceil(sourceBytes);
}

static void NormalizeOrDefault(VxVector &value, float x, float y, float z)
{
    float len = value.SquareMagnitude();
    if (len > 0.000001f) {
        value *= 1.0f / sqrtf(len);
    } else {
        value.Set(x, y, z);
    }
}

static float DirectionToPan(const VxVector &direction)
{
    float len = sqrtf(direction.x * direction.x + direction.z * direction.z);
    if (len <= 0.000001f) return 0.0f;
    return ClampFloat(direction.x / len, -1.0f, 1.0f);
}

static void ComputeConstantPowerPan(float pan, float &leftGain, float &rightGain)
{
    pan = ClampFloat(pan, -1.0f, 1.0f);
    leftGain = sqrtf(0.5f * (1.0f - pan));
    rightGain = sqrtf(0.5f * (1.0f + pan));
}

static void ComputeStereoVirtualPans(const VxVector &direction, float &leftPan, float &rightPan)
{
    float x = direction.x;
    float z = direction.z;
    float len = sqrtf(x * x + z * z);
    const float sin30 = 0.5f;
    const float cos30 = 0.8660254037844386f;

    if (len > 0.000001f) {
        x /= len;
        z /= len;
    } else {
        x = 0.0f;
        z = 1.0f;
    }

    leftPan = ClampFloat(x * cos30 - z * sin30, -1.0f, 1.0f);
    rightPan = ClampFloat(x * cos30 + z * sin30, -1.0f, 1.0f);
}

static CKBOOL ComputeOrientedStereoVirtualPans(const VxVector &direction,
                                               VxVector sourceDirection, VxVector sourceUp,
                                               float &leftPan, float &rightPan)
{
    VxVector sourceRight;
    VxVector leftDirection;
    VxVector rightDirection;
    float len;
    const float spread = 0.5f;

    len = sourceDirection.SquareMagnitude();
    if (len <= 0.000001f) return FALSE;
    sourceDirection *= 1.0f / sqrtf(len);

    len = sourceUp.SquareMagnitude();
    if (len <= 0.000001f) return FALSE;
    sourceUp *= 1.0f / sqrtf(len);
    sourceUp -= sourceDirection * DotProduct(sourceUp, sourceDirection);

    len = sourceUp.SquareMagnitude();
    if (len <= 0.000001f) return FALSE;
    sourceUp *= 1.0f / sqrtf(len);

    sourceRight = CrossProduct(sourceUp, sourceDirection);
    len = sourceRight.SquareMagnitude();
    if (len <= 0.000001f) return FALSE;
    sourceRight *= 1.0f / sqrtf(len);

    leftDirection = direction - sourceRight * spread;
    rightDirection = direction + sourceRight * spread;
    leftPan = DirectionToPan(leftDirection);
    rightPan = DirectionToPan(rightDirection);
    return TRUE;
}

static void NormalizeStereoMixMatrix(float &leftToLeft, float &leftToRight,
                                     float &rightToLeft, float &rightToRight)
{
    float leftOutGain = leftToLeft + rightToLeft;
    float rightOutGain = leftToRight + rightToRight;
    float maxOutGain = leftOutGain;

    if (rightOutGain > maxOutGain) {
        maxOutGain = rightOutGain;
    }
    if (maxOutGain > 1.0f) {
        float scale = 1.0f / maxOutGain;
        leftToLeft *= scale;
        leftToRight *= scale;
        rightToLeft *= scale;
        rightToRight *= scale;
    }
}

CKERROR CreateSdlSoundManager(CKContext *context)
{
    if (!context) return CKERR_INVALIDPARAMETER;
    
    SdlSoundManager* manager = new SdlSoundManager(context);
    if (!manager) return CKERR_OUTOFMEMORY;
    
    return CK_OK;
}

CKERROR CreateNewManager(CKContext *context)
{
    if (!context) return CKERR_INVALIDPARAMETER;
    return CreateSdlSoundManager(context);
}

CKERROR RemoveManager(CKContext *context)
{
    SdlSoundManager *manager;
    
    if (!context) return CKERR_INVALIDPARAMETER;
    
    manager = (SdlSoundManager*)context->GetManagerByName(SdlSoundManagerName);
    if (manager) {
        delete manager;
    }
    return CK_OK;
}

PLUGIN_EXPORT CKPluginInfo *CKGetPluginInfo(int Index)
{
    g_PluginInfo.m_Author = "Ballanced";
    g_PluginInfo.m_Description = "SDL3 Sound Manager";
    g_PluginInfo.m_Extension = "";
    g_PluginInfo.m_Type = CKPLUGIN_MANAGER_DLL;
    g_PluginInfo.m_Version = 0x000001;
    g_PluginInfo.m_InitInstanceFct = CreateNewManager;
    g_PluginInfo.m_ExitInstanceFct = RemoveManager;
    g_PluginInfo.m_GUID = SDL_SOUNDMANAGER_GUID;
    g_PluginInfo.m_Summary = SdlSoundManagerName;
    return &g_PluginInfo;
}

//-----------------------------------------------------------------------------
// Audio Conversion Utility Functions
//-----------------------------------------------------------------------------

float SdlSoundManager::DbToLinear(float db)
{
    if (db <= -100.0f) {
        return 0.0f;
    }
    if (db >= 0.0f) {
        return 1.0f;
    }
    return powf(10.0f, db / 20.0f);
}

float SdlSoundManager::LinearToDb(float linear)
{
    if (linear <= 0.0f) {
        return -100.0f;
    }
    if (linear >= 1.0f) {
        return 0.0f;
    }
    return 20.0f * log10f(linear);
}

SDL_AudioFormat SdlSoundManager::CKFormatToSDL(const CKWaveFormat &wf)
{
    // Convert CKWaveFormat to SDL_AudioFormat
    switch (wf.wBitsPerSample) {
        case 8:
            return SDL_AUDIO_U8;
        case 16:
            return SDL_AUDIO_S16LE;
        case 32:
            return SDL_AUDIO_S32LE;
        default:
            return SDL_AUDIO_S16LE;
    }
}

//-----------------------------------------------------------------------------
// SdlSoundSource Implementation
//-----------------------------------------------------------------------------

SdlSoundSource::SdlSoundSource()
{
    m_Stream = NULL;
    m_Buffer = NULL;
    m_BufferSize = 0;
    m_MixBuffer = NULL;
    m_MixBufferSize = 0;
    m_WritePos = 0;
    m_PlayPos = 0;
    m_Type = CK_WAVESOUND_BACKGROUND;
    m_IsPlaying = FALSE;
    m_IsPaused = FALSE;
    m_IsLooping = FALSE;
    m_IsStreamed = FALSE;
    m_Manager = NULL;
    m_TotalBytesPushed = 0;
    m_IsBound = FALSE;
    
    memset(&m_Format, 0, sizeof(CKWaveFormat));
    m_Format.wFormatTag = CKWAVE_FORMAT_PCM;
    m_Format.nChannels = 2;
    m_Format.nSamplesPerSec = SDL_DEFAULT_SAMPLE_RATE;
    m_Format.wBitsPerSample = 16;
    m_Format.nBlockAlign = 4;
    m_Format.nAvgBytesPerSec = SDL_DEFAULT_SAMPLE_RATE * 4;
    ResetMixMatrix();
}

SdlSoundSource::~SdlSoundSource()
{
    Cleanup();
}

void SdlSoundSource::Reset()
{
    m_WritePos = 0;
    m_PlayPos = 0;
    m_IsPlaying = FALSE;
    m_IsPaused = FALSE;
    m_TotalBytesPushed = 0;
    ResetMixMatrix();
    
    if (m_Buffer && m_BufferSize > 0) {
        memset(m_Buffer, 0, m_BufferSize);
    }
}

CKBOOL SdlSoundSource::Initialize(SdlSoundManager *manager, CK_WAVESOUND_TYPE type, 
                                  const CKWaveFormat *wf, CKDWORD bytes, CKBOOL streamed)
{
    SDL_AudioSpec spec;
    SDL_AudioSpec streamSpec;
    
    if (!manager || !wf || bytes == 0) return FALSE;
    
    Cleanup();
    
    m_Manager = manager;
    m_Type = type;
    m_IsStreamed = streamed;
    memcpy(&m_Format, wf, sizeof(CKWaveFormat));
    m_TotalBytesPushed = 0;
    m_IsBound = FALSE;
    ResetMixMatrix();
    
    // Allocate ring buffer
    m_BufferSize = bytes;
    m_Buffer = new CKBYTE[bytes];
    if (!m_Buffer) return FALSE;
    memset(m_Buffer, 0, bytes);
    
    // Ensure audio device exists
    if (!manager->ValidateMixer()) {
        Cleanup();
        return FALSE;
    }
    
    // Create audio stream for feeding data
    spec.format = SdlSoundManager::CKFormatToSDL(m_Format);
    spec.channels = m_Format.nChannels;
    spec.freq = m_Format.nSamplesPerSec;
    streamSpec = spec;
    if (streamSpec.channels == 1) {
        streamSpec.channels = 2;
    }
    
    m_Stream = SDL_CreateAudioStream(&streamSpec, &streamSpec);
    if (!m_Stream) {
        Cleanup();
        return FALSE;
    }
    
    // Apply initial settings
    ApplySettings();
    
    return TRUE;
}

void SdlSoundSource::Cleanup()
{
    if (m_Stream) {
        if (m_IsBound) {
            SDL_UnbindAudioStream(m_Stream);
            m_IsBound = FALSE;
        }
        SDL_DestroyAudioStream(m_Stream);
        m_Stream = NULL;
    }
    
    if (m_Buffer) {
        delete[] m_Buffer;
        m_Buffer = NULL;
    }

    if (m_MixBuffer) {
        delete[] m_MixBuffer;
        m_MixBuffer = NULL;
    }
    
    m_BufferSize = 0;
    m_MixBufferSize = 0;
    m_Manager = NULL;
    m_TotalBytesPushed = 0;
    ResetMixMatrix();
}

void SdlSoundSource::ResetMixMatrix()
{
    m_LeftToLeftGain = 1.0f;
    m_RightToLeftGain = 0.0f;
    if (m_Format.nChannels == 1) {
        m_LeftToRightGain = 1.0f;
        m_RightToRightGain = 0.0f;
    } else {
        m_LeftToRightGain = 0.0f;
        m_RightToRightGain = 1.0f;
    }
    ForgetQueuedMixMatrix();
}

void SdlSoundSource::ForgetQueuedMixMatrix()
{
    m_QueuedLeftToLeftGain = m_LeftToLeftGain;
    m_QueuedLeftToRightGain = m_LeftToRightGain;
    m_QueuedRightToLeftGain = m_RightToLeftGain;
    m_QueuedRightToRightGain = m_RightToRightGain;
    m_HasQueuedMixMatrix = FALSE;
}

void SdlSoundSource::RememberQueuedMixMatrix()
{
    m_QueuedLeftToLeftGain = m_LeftToLeftGain;
    m_QueuedLeftToRightGain = m_LeftToRightGain;
    m_QueuedRightToLeftGain = m_RightToLeftGain;
    m_QueuedRightToRightGain = m_RightToRightGain;
    m_HasQueuedMixMatrix = TRUE;
}

CKBOOL SdlSoundSource::QueuedMixMatrixChanged() const
{
    if (!m_HasQueuedMixMatrix) return FALSE;
    return MixMatrixChanged(m_QueuedLeftToLeftGain, m_QueuedLeftToRightGain,
                            m_QueuedRightToLeftGain, m_QueuedRightToRightGain,
                            m_LeftToLeftGain, m_LeftToRightGain,
                            m_RightToLeftGain, m_RightToRightGain);
}

void SdlSoundSource::QueueAudioData(const void *data, CKDWORD bytes)
{
    CKBYTE *mixed;
    CKDWORD i;
    CKDWORD frameBytes;
    CKDWORD mixedBytes;
    Uint8 *u8;
    const Uint8 *srcU8;
    Sint16 *s16;
    const Sint16 *srcS16;
    Sint32 *s32;
    const Sint32 *srcS32;
    float leftSample;
    float rightSample;
    double leftWide;
    double rightWide;

    if (!m_Stream || !data || bytes == 0) return;

    if (m_Format.nBlockAlign == 0 ||
        (bytes % m_Format.nBlockAlign) != 0 ||
        (m_Format.nChannels != 1 && m_Format.nChannels != 2) ||
        (m_Format.wBitsPerSample != 8 && m_Format.wBitsPerSample != 16 && m_Format.wBitsPerSample != 32)) {
        SDL_PutAudioStreamData(m_Stream, data, (int)bytes);
        if (!m_HasQueuedMixMatrix) RememberQueuedMixMatrix();
        return;
    }

    if (m_Format.nChannels == 2 &&
        m_LeftToLeftGain == 1.0f && m_LeftToRightGain == 0.0f &&
        m_RightToLeftGain == 0.0f && m_RightToRightGain == 1.0f) {
        SDL_PutAudioStreamData(m_Stream, data, (int)bytes);
        if (!m_HasQueuedMixMatrix) RememberQueuedMixMatrix();
        return;
    }

    mixedBytes = (m_Format.nChannels == 1) ? (bytes * 2) : bytes;
    if (mixedBytes > m_MixBufferSize) {
        if (m_MixBuffer) {
            delete[] m_MixBuffer;
            m_MixBuffer = NULL;
            m_MixBufferSize = 0;
        }
        m_MixBuffer = new CKBYTE[mixedBytes];
        if (!m_MixBuffer) {
            SDL_PutAudioStreamData(m_Stream, data, (int)bytes);
            if (!m_HasQueuedMixMatrix) RememberQueuedMixMatrix();
            return;
        }
        m_MixBufferSize = mixedBytes;
    }
    mixed = m_MixBuffer;

    frameBytes = m_Format.nBlockAlign;

    if (m_Format.nChannels == 1) {
        switch (m_Format.wBitsPerSample) {
            case 8:
                srcU8 = (const Uint8 *)data;
                u8 = (Uint8 *)mixed;
                for (i = 0; i < bytes; ++i) {
                    leftSample = (float)((int)srcU8[i] - 128);
                    u8[i * 2] = (Uint8)ClampFloat(leftSample * m_LeftToLeftGain + 128.0f, 0.0f, 255.0f);
                    u8[i * 2 + 1] = (Uint8)ClampFloat(leftSample * m_LeftToRightGain + 128.0f, 0.0f, 255.0f);
                }
                break;

            case 16:
                srcS16 = (const Sint16 *)data;
                s16 = (Sint16 *)mixed;
                for (i = 0; i < bytes / sizeof(Sint16); ++i) {
                    leftSample = (float)srcS16[i];
                    s16[i * 2] = ClampSint16(leftSample * m_LeftToLeftGain);
                    s16[i * 2 + 1] = ClampSint16(leftSample * m_LeftToRightGain);
                }
                break;

            case 32:
                srcS32 = (const Sint32 *)data;
                s32 = (Sint32 *)mixed;
                for (i = 0; i < bytes / sizeof(Sint32); ++i) {
                    leftWide = (double)srcS32[i];
                    s32[i * 2] = ClampSint32(leftWide * (double)m_LeftToLeftGain);
                    s32[i * 2 + 1] = ClampSint32(leftWide * (double)m_LeftToRightGain);
                }
                break;
        }
    } else {
        memcpy(mixed, data, bytes);

        switch (m_Format.wBitsPerSample) {
            case 8:
                for (i = 0; i < bytes; i += frameBytes) {
                    u8 = (Uint8 *)(mixed + i);
                    leftSample = (float)((int)u8[0] - 128);
                    rightSample = (float)((int)u8[1] - 128);
                    u8[0] = (Uint8)ClampFloat(leftSample * m_LeftToLeftGain +
                                               rightSample * m_RightToLeftGain + 128.0f, 0.0f, 255.0f);
                    u8[1] = (Uint8)ClampFloat(leftSample * m_LeftToRightGain +
                                               rightSample * m_RightToRightGain + 128.0f, 0.0f, 255.0f);
                }
                break;

            case 16:
                for (i = 0; i < bytes; i += frameBytes) {
                    s16 = (Sint16 *)(mixed + i);
                    leftSample = (float)s16[0];
                    rightSample = (float)s16[1];
                    s16[0] = ClampSint16(leftSample * m_LeftToLeftGain +
                                         rightSample * m_RightToLeftGain);
                    s16[1] = ClampSint16(leftSample * m_LeftToRightGain +
                                         rightSample * m_RightToRightGain);
                }
                break;

            case 32:
                for (i = 0; i < bytes; i += frameBytes) {
                    s32 = (Sint32 *)(mixed + i);
                    leftWide = (double)s32[0];
                    rightWide = (double)s32[1];
                    s32[0] = ClampSint32(leftWide * (double)m_LeftToLeftGain +
                                         rightWide * (double)m_RightToLeftGain);
                    s32[1] = ClampSint32(leftWide * (double)m_LeftToRightGain +
                                         rightWide * (double)m_RightToRightGain);
                }
                break;
        }
    }

    SDL_PutAudioStreamData(m_Stream, mixed, (int)mixedBytes);
    if (!m_HasQueuedMixMatrix) RememberQueuedMixMatrix();
}

int SdlSoundSource::GetPendingSourceBytes() const
{
    int queuedBytes;
    int availableBytes;
    int queuedSourceBytes;
    int availableSourceBytes;
    float ratio;
    Sint64 pendingBytes;

    if (!m_Stream) return 0;

    queuedBytes = SDL_GetAudioStreamQueued(m_Stream);
    availableBytes = SDL_GetAudioStreamAvailable(m_Stream);
    if (queuedBytes < 0 || availableBytes < 0) return -1;

    queuedSourceBytes = StreamBytesToSourceBytes(queuedBytes, m_Format, 1.0f);

    ratio = SDL_GetAudioStreamFrequencyRatio(m_Stream);
    if (ratio <= 0.0f) ratio = 1.0f;
    availableSourceBytes = StreamBytesToSourceBytes(availableBytes, m_Format, ratio);

    pendingBytes = (Sint64)queuedSourceBytes + (Sint64)availableSourceBytes;
    if (pendingBytes > 2147483647) return 2147483647;
    return (int)pendingBytes;
}

void SdlSoundSource::RefreshQueuedAudio()
{
    Uint64 queued;
    Uint64 consumed;
    CKDWORD playPos;
    CKDWORD startPos;
    CKDWORD tailBytes;
    CKDWORD queuedSourceBytes;
    int queuedBytes;

    if (!m_IsPlaying || !m_Stream || !m_Buffer || m_BufferSize == 0) return;

    queuedBytes = GetPendingSourceBytes();
    if (queuedBytes < 0) return;
    queuedSourceBytes = (CKDWORD)queuedBytes;

    if (m_IsStreamed) {
        if (queuedSourceBytes == 0 || queuedSourceBytes > m_BufferSize) return;

        startPos = (m_WritePos + m_BufferSize - queuedSourceBytes) % m_BufferSize;
        SDL_ClearAudioStream(m_Stream);
        ForgetQueuedMixMatrix();

        tailBytes = min(queuedSourceBytes, m_BufferSize - startPos);
        if (tailBytes > 0) {
            QueueAudioData(m_Buffer + startPos, tailBytes);
        }
        if (queuedSourceBytes > tailBytes) {
            QueueAudioData(m_Buffer, queuedSourceBytes - tailBytes);
        }
        SDL_FlushAudioStream(m_Stream);
        return;
    }

    queued = (Uint64)queuedBytes;
    consumed = (m_TotalBytesPushed > queued) ? (m_TotalBytesPushed - queued) : 0;
    playPos = (CKDWORD)((m_PlayPos + consumed) % m_BufferSize);

    SDL_ClearAudioStream(m_Stream);
    ForgetQueuedMixMatrix();
    m_TotalBytesPushed = 0;
    m_PlayPos = playPos;

    tailBytes = m_BufferSize - playPos;
    if (tailBytes > 0) {
        QueueAudioData(m_Buffer + playPos, tailBytes);
        m_TotalBytesPushed += tailBytes;
    }
    if (m_IsLooping && playPos > 0) {
        QueueAudioData(m_Buffer, playPos);
        m_TotalBytesPushed += playPos;
    }
    SDL_FlushAudioStream(m_Stream);
}

void SdlSoundSource::ApplySettings()
{
    float leftGain, rightGain;
    float pan;
    float pitch;

    if (!m_Stream) return;

    pitch = m_Settings.m_Pitch;
    if (pitch <= 0.0f) pitch = 1.0f;
    SDL_SetAudioStreamFrequencyRatio(m_Stream, pitch);

    // Base gain; 3D attenuation (if any) is applied in Apply3DSettings.
    SDL_SetAudioStreamGain(m_Stream, m_Settings.m_Gain);

    if (m_Type == CK_WAVESOUND_BACKGROUND &&
        (m_Format.nChannels == 1 || m_Format.nChannels == 2)) {
        pan = ClampFloat(m_Settings.m_Pan, -1.0f, 1.0f);
        leftGain = (pan > 0.0f) ? (1.0f - pan) : 1.0f;
        rightGain = (pan < 0.0f) ? (1.0f + pan) : 1.0f;
        m_LeftToLeftGain = leftGain;
        m_LeftToRightGain = (m_Format.nChannels == 1) ? rightGain : 0.0f;
        m_RightToLeftGain = 0.0f;
        m_RightToRightGain = (m_Format.nChannels == 2) ? rightGain : 0.0f;
    } else if (m_Type == CK_WAVESOUND_BACKGROUND) {
        m_LeftToLeftGain = 1.0f;
        m_LeftToRightGain = 0.0f;
        m_RightToLeftGain = 0.0f;
        m_RightToRightGain = 1.0f;
    }

    if (QueuedMixMatrixChanged()) {
        RefreshQueuedAudio();
    }
}

void SdlSoundSource::Apply3DSettings(const CKListenerSettings &listener)
{
    VxVector relative;
    VxVector listenerPosition;
    VxVector listenerVelocity;
    VxVector listenerRight;
    VxVector listenerUp;
    VxVector listenerForward;
    VxVector sourceVelocity;
    VxVector sourceDirection;
    VxVector sourceUp;
    VxVector worldRelative;
    VxVector dirToSource;
    VxVector toListener;
    VxVector coneDir;
    float distance, attenuation;
    float px, py, pz;
    float minDistance, maxDistance;
    float rolloff, clampedDistance;
    float len, invLen;
    float pan;
    float coneGain;
    float dot, angle;
    float inAngle, outAngle, angleRange;
    float speed, numer, denom;
    float sourceToward, listenerTowardSource;
    float doppler, pitch;
    float leftGain, rightGain;
    float leftPan, rightPan;
    float leftChannelLeftGain, leftChannelRightGain;
    float rightChannelLeftGain, rightChannelRightGain;

    if (!m_Stream) return;
    
    // Only apply 3D for non-background sounds
    if (m_Type == CK_WAVESOUND_BACKGROUND) return;

    listenerPosition.Set(0.0f, 0.0f, 0.0f);
    listenerVelocity.Set(0.0f, 0.0f, 0.0f);
    listenerRight.Set(1.0f, 0.0f, 0.0f);
    listenerUp.Set(0.0f, 1.0f, 0.0f);
    listenerForward.Set(0.0f, 0.0f, 1.0f);

    if (m_Manager) {
        listenerPosition = m_Manager->m_LastListenerPosition;
        listenerVelocity = m_Manager->m_LastListenerVelocity;
        listenerRight = m_Manager->m_LastListenerRight;
        listenerUp = m_Manager->m_LastListenerUp;
        listenerForward = m_Manager->m_LastListenerForward;
    }

    NormalizeOrDefault(listenerRight, 1.0f, 0.0f, 0.0f);
    NormalizeOrDefault(listenerUp, 0.0f, 1.0f, 0.0f);
    NormalizeOrDefault(listenerForward, 0.0f, 0.0f, 1.0f);

    if (m_3DSettings.m_HeadRelative) {
        relative = m_3DSettings.m_Position;
        sourceVelocity = m_3DSettings.m_Velocity;
        sourceDirection = m_3DSettings.m_OrientationDir;
        sourceUp = m_3DSettings.m_OrientationUp;
        listenerVelocity.Set(0.0f, 0.0f, 0.0f);
    } else {
        worldRelative = m_3DSettings.m_Position - listenerPosition;
        relative.Set(DotProduct(worldRelative, listenerRight),
                     DotProduct(worldRelative, listenerUp),
                     DotProduct(worldRelative, listenerForward));
        sourceVelocity.Set(DotProduct(m_3DSettings.m_Velocity, listenerRight),
                           DotProduct(m_3DSettings.m_Velocity, listenerUp),
                           DotProduct(m_3DSettings.m_Velocity, listenerForward));
        sourceDirection.Set(DotProduct(m_3DSettings.m_OrientationDir, listenerRight),
                            DotProduct(m_3DSettings.m_OrientationDir, listenerUp),
                            DotProduct(m_3DSettings.m_OrientationDir, listenerForward));
        sourceUp.Set(DotProduct(m_3DSettings.m_OrientationUp, listenerRight),
                     DotProduct(m_3DSettings.m_OrientationUp, listenerUp),
                     DotProduct(m_3DSettings.m_OrientationUp, listenerForward));
        listenerVelocity.Set(DotProduct(listenerVelocity, listenerRight),
                             DotProduct(listenerVelocity, listenerUp),
                             DotProduct(listenerVelocity, listenerForward));
    }

    px = relative.x * listener.m_DistanceFactor;
    py = relative.y * listener.m_DistanceFactor;
    pz = relative.z * listener.m_DistanceFactor;

    // Calculate distance for custom attenuation
    distance = sqrtf(px * px + py * py + pz * pz);
    minDistance = m_3DSettings.m_MinDistance;
    maxDistance = m_3DSettings.m_MaxDistance;
    rolloff = listener.m_RollOff;
    if (minDistance < 0.000001f) minDistance = 0.000001f;
    if (maxDistance < minDistance) maxDistance = minDistance;
    if (rolloff < 0.0f) rolloff = 0.0f;
    
    // Apply min/max distance attenuation
    if (distance < minDistance || rolloff == 0.0f) {
        attenuation = 1.0f;
    } else if (distance > maxDistance && m_3DSettings.m_MuteAfterMax) {
        attenuation = 0.0f;
    } else {
        clampedDistance = (distance > maxDistance) ? maxDistance : distance;
        attenuation = minDistance / (minDistance + rolloff * (clampedDistance - minDistance));
        attenuation = ClampFloat(attenuation, 0.0f, 1.0f);
    }

    coneGain = 1.0f;
    len = sourceDirection.SquareMagnitude();
    if (len > 0.000001f && distance > 0.000001f && m_3DSettings.m_OutAngle < 360.0f) {
        coneDir = sourceDirection;
        coneDir *= 1.0f / sqrtf(len);

        toListener.Set(-relative.x, -relative.y, -relative.z);
        toListener.Normalize();

        dot = ClampFloat(DotProduct(coneDir, toListener), -1.0f, 1.0f);
        angle = acosf(dot) * 180.0f / SDL_SOUND_PI;
        inAngle = m_3DSettings.m_InAngle * 0.5f;
        outAngle = m_3DSettings.m_OutAngle * 0.5f;

        if (outAngle < inAngle) outAngle = inAngle;

        if (angle >= outAngle) {
            coneGain = ClampFloat(m_3DSettings.m_OutsideGain, 0.0f, 1.0f);
        } else if (angle > inAngle) {
            angleRange = outAngle - inAngle;
            if (angleRange > 0.000001f) {
                coneGain = 1.0f - ((angle - inAngle) / angleRange) *
                           (1.0f - ClampFloat(m_3DSettings.m_OutsideGain, 0.0f, 1.0f));
            }
        }
    }

    pan = 0.0f;
    len = relative.SquareMagnitude();
    if (len > 0.000001f) {
        invLen = 1.0f / sqrtf(len);
        dirToSource = relative * invLen;
        pan = DirectionToPan(dirToSource);
    } else {
        dirToSource.Set(0.0f, 0.0f, 1.0f);
    }
    if (m_Format.nChannels == 1) {
        ComputeConstantPowerPan(pan, leftGain, rightGain);
        m_LeftToLeftGain = leftGain;
        m_LeftToRightGain = rightGain;
        m_RightToLeftGain = 0.0f;
        m_RightToRightGain = 0.0f;
    } else {
        if (!ComputeOrientedStereoVirtualPans(dirToSource, sourceDirection, sourceUp,
                                              leftPan, rightPan)) {
            ComputeStereoVirtualPans(dirToSource, leftPan, rightPan);
        }
        ComputeConstantPowerPan(leftPan, leftChannelLeftGain, leftChannelRightGain);
        ComputeConstantPowerPan(rightPan, rightChannelLeftGain, rightChannelRightGain);
        m_LeftToLeftGain = leftChannelLeftGain;
        m_LeftToRightGain = leftChannelRightGain;
        m_RightToLeftGain = rightChannelLeftGain;
        m_RightToRightGain = rightChannelRightGain;
        NormalizeStereoMixMatrix(m_LeftToLeftGain, m_LeftToRightGain,
                                 m_RightToLeftGain, m_RightToRightGain);
    }
    if (QueuedMixMatrixChanged()) {
        RefreshQueuedAudio();
    }

    doppler = 1.0f;
    if (listener.m_DopplerFactor != 0.0f && len > 0.000001f) {
        toListener.Set(-dirToSource.x, -dirToSource.y, -dirToSource.z);
        sourceToward = DotProduct(sourceVelocity, toListener);
        listenerTowardSource = m_3DSettings.m_HeadRelative ? 0.0f : DotProduct(listenerVelocity, dirToSource);
        speed = SDL_SOUND_SPEED;
        if (listener.m_DistanceFactor > 0.000001f) {
            speed /= listener.m_DistanceFactor;
        }

        numer = speed + listenerTowardSource * listener.m_DopplerFactor;
        denom = speed - sourceToward * listener.m_DopplerFactor;
        if (denom < speed * 0.1f) denom = speed * 0.1f;
        if (numer < speed * 0.1f) numer = speed * 0.1f;
        doppler = ClampFloat(numer / denom, 0.5f, 2.0f);
    }

    pitch = m_Settings.m_Pitch * doppler;
    if (pitch <= 0.0f) pitch = 1.0f;
    SDL_SetAudioStreamFrequencyRatio(m_Stream, pitch);

    // Apply attenuation via per-stream gain (global gain handled at device)
    SDL_SetAudioStreamGain(m_Stream, m_Settings.m_Gain * attenuation * coneGain);
}

//-----------------------------------------------------------------------------
// SdlSoundManager Constructor/Destructor
//-----------------------------------------------------------------------------

SdlSoundManager::SdlSoundManager(CKContext *Context) 
    : CKSoundManager(Context, SdlSoundManagerName)
{
    m_Device = 0;
    m_bInitialized = FALSE;
    m_Mutex = NULL;
    m_LastListenerPosition.Set(0.0f, 0.0f, 0.0f);
    m_LastListenerVelocity.Set(0.0f, 0.0f, 0.0f);
    m_LastListenerRight.Set(1.0f, 0.0f, 0.0f);
    m_LastListenerUp.Set(0.0f, 1.0f, 0.0f);
    m_LastListenerForward.Set(0.0f, 0.0f, 1.0f);
    
    m_Mutex = SDL_CreateMutex();
    m_Context->RegisterNewManager(this);
}

SdlSoundManager::~SdlSoundManager()
{
    ShutdownAudio(FALSE);
    
    if (m_Mutex) {
        SDL_DestroyMutex(m_Mutex);
        m_Mutex = NULL;
    }
}

//-----------------------------------------------------------------------------
// Thread Safety Helpers
//-----------------------------------------------------------------------------

void SdlSoundManager::LockMutex() const
{
    if (m_Mutex) {
        SDL_LockMutex(m_Mutex);
    }
}

void SdlSoundManager::UnlockMutex() const
{
    if (m_Mutex) {
        SDL_UnlockMutex(m_Mutex);
    }
}

//-----------------------------------------------------------------------------
// Capability and Status Methods
//-----------------------------------------------------------------------------

CK_SOUNDMANAGER_CAPS SdlSoundManager::GetCaps()
{
    CKDWORD caps = CK_WAVESOUND_SETTINGS_ALL | 
                   CK_WAVESOUND_3DSETTINGS_ALL | 
                   CK_LISTENERSETTINGS_ALL | 
                   CK_WAVESOUND_3DSETTINGS_DISTANCEFACTOR | 
                   CK_WAVESOUND_3DSETTINGS_DOPPLERFACTOR;
    
    // Remove unsupported features
    caps &= ~(CK_WAVESOUND_SETTINGS_EQUALIZATION | 
              CK_WAVESOUND_SETTINGS_PRIORITY | 
              CK_LISTENERSETTINGS_EQ | 
              CK_LISTENERSETTINGS_PRIORITY | 
              CK_SOUNDMANAGER_ONFLYTYPE);
    
    return (CK_SOUNDMANAGER_CAPS)caps;
}

CKBOOL SdlSoundManager::IsInitialized()
{
    CKBOOL result;
    LockMutex();
    result = m_bInitialized && m_Device;
    UnlockMutex();
    return result;
}

//-----------------------------------------------------------------------------
// Validation Methods
//-----------------------------------------------------------------------------

CKBOOL SdlSoundManager::ValidateSource(void *source) const
{
    return source != NULL;
}

CKBOOL SdlSoundManager::ValidateMixer() const
{
    return m_Device != 0;
}

//-----------------------------------------------------------------------------
// Source Creation and Management
//-----------------------------------------------------------------------------

void *SdlSoundManager::CreateSource(CK_WAVESOUND_TYPE type, CKWaveFormat *wf, CKDWORD bytes, CKBOOL streamed)
{
    SdlSoundSource *source;
    
    if (!wf || bytes == 0) return NULL;
    
    if (m_Context->GetStartOptions() & CK_CONFIG_DISABLEDSOUND) {
        if (m_Context->IsInInterfaceMode()) {
            m_Context->OutputToConsole("Cannot create sound: Sound disabled");
        }
        return NULL;
    }

    if (!ValidateMixer()) {
        if (m_Context->IsInInterfaceMode()) {
            m_Context->OutputToConsole("Cannot create sound: Sound manager not initialized");
        }
        return NULL;
    }

    source = new SdlSoundSource();
    if (!source) return NULL;
    
    if (!source->Initialize(this, type, wf, bytes, streamed)) {
        delete source;
        return NULL;
    }
    
    return source;
}

void *SdlSoundManager::DuplicateSource(void *source)
{
    SdlSoundSource *srcSource;
    SdlSoundSource *newSource;
    
    if (!ValidateSource(source) || !ValidateMixer()) {
        return NULL;
    }

    srcSource = (SdlSoundSource*)source;
    newSource = new SdlSoundSource();
    if (!newSource) return NULL;
    
    // Initialize with same parameters
    if (!newSource->Initialize(this, srcSource->m_Type, &srcSource->m_Format, 
                               srcSource->m_BufferSize, srcSource->m_IsStreamed)) {
        delete newSource;
        return NULL;
    }
    
    // Copy buffer content
    if (srcSource->m_Buffer && newSource->m_Buffer && srcSource->m_BufferSize > 0) {
        memcpy(newSource->m_Buffer, srcSource->m_Buffer, srcSource->m_BufferSize);
    }
    
    // Copy settings
    newSource->m_Settings = srcSource->m_Settings;
    newSource->m_3DSettings = srcSource->m_3DSettings;
    newSource->ApplySettings();
    newSource->Apply3DSettings(m_ListenerSettings);
    
    return newSource;
}

void SdlSoundManager::ReleaseSource(void *source)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return;
    
    sdlSource = (SdlSoundSource*)source;
    delete sdlSource;
}

//-----------------------------------------------------------------------------
// Playback Control
//-----------------------------------------------------------------------------

void SdlSoundManager::InternalPause(void *source)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return;
    
    sdlSource = (SdlSoundSource*)source;
    if (sdlSource->m_Stream && sdlSource->m_IsBound) {
        SDL_UnbindAudioStream(sdlSource->m_Stream);
        sdlSource->m_IsBound = FALSE;
    }
    sdlSource->m_IsPaused = TRUE;
}

void SdlSoundManager::InternalPlay(void *source, CKBOOL loop)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return;
    
    sdlSource = (SdlSoundSource*)source;
    if (!ValidateMixer() || !sdlSource->m_Stream) return;

    if (!sdlSource->m_IsBound) {
        if (!SDL_BindAudioStream(m_Device, sdlSource->m_Stream)) {
            if (m_Context->IsInInterfaceMode()) {
                m_Context->OutputToConsole("Failed to bind SDL audio stream");
            }
            return;
        }
        sdlSource->m_IsBound = TRUE;
    }

    sdlSource->ApplySettings();
    sdlSource->Apply3DSettings(m_ListenerSettings);
    
    // For non-streamed sounds, put entire buffer in stream
    if (!sdlSource->m_IsStreamed && sdlSource->m_Buffer && sdlSource->m_BufferSize > 0) {
        // Clear any existing data
        SDL_ClearAudioStream(sdlSource->m_Stream);
        sdlSource->ForgetQueuedMixMatrix();
        sdlSource->m_TotalBytesPushed = 0;

        const CKDWORD startPos = min((CKDWORD)max(0, sdlSource->m_PlayPos), sdlSource->m_BufferSize);
        const CKDWORD tailBytes = sdlSource->m_BufferSize - startPos;

        if (tailBytes > 0) {
            sdlSource->QueueAudioData(sdlSource->m_Buffer + startPos, tailBytes);
            sdlSource->m_TotalBytesPushed += tailBytes;
        }
        if (loop && startPos > 0) {
            sdlSource->QueueAudioData(sdlSource->m_Buffer, startPos);
            sdlSource->m_TotalBytesPushed += startPos;
        }
        SDL_FlushAudioStream(sdlSource->m_Stream);
    }
    
    sdlSource->m_IsPlaying = TRUE;
    sdlSource->m_IsPaused = FALSE;
    sdlSource->m_IsLooping = loop;
}

void SdlSoundManager::Play(CKWaveSound *ws, void *source, CKBOOL loop)
{
    SdlSoundSource *sdlSource = NULL;
    SoundMinion *minion;
    
    if (!ValidateSource(source)) return;
    
    if (ws) {
        // Normal sound
        sdlSource = (SdlSoundSource*)source;
        m_SoundsPlaying.AddIfNotHere(ws->GetID());
    } else {
        // Minion sound
        minion = (SoundMinion*)source;
        if (minion && minion->m_Source) {
            sdlSource = (SdlSoundSource*)minion->m_Source;
        }
    }
    
    if (sdlSource) {
        InternalPlay(sdlSource, loop);
    }
}

void SdlSoundManager::Pause(CKWaveSound *ws, void *source)
{
    if (!ValidateSource(source)) return;
    InternalPause(source);
}

void SdlSoundManager::SetPlayPosition(void *source, int pos)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source) || pos < 0) return;
    
    sdlSource = (SdlSoundSource*)source;
    sdlSource->m_PlayPos = (CKDWORD)pos;

    if (sdlSource->m_IsStreamed && sdlSource->m_Stream) {
        SDL_ClearAudioStream(sdlSource->m_Stream);
        sdlSource->ForgetQueuedMixMatrix();
        sdlSource->m_TotalBytesPushed = 0;
        sdlSource->m_PlayPos = 0;
        sdlSource->m_WritePos = 0;
        return;
    }

    if (!sdlSource->m_IsStreamed && sdlSource->m_Stream && sdlSource->m_IsPlaying) {
        SDL_ClearAudioStream(sdlSource->m_Stream);
        sdlSource->ForgetQueuedMixMatrix();
        sdlSource->m_TotalBytesPushed = 0;

        const CKDWORD startPos = min((CKDWORD)pos, sdlSource->m_BufferSize);
        const CKDWORD tailBytes = sdlSource->m_BufferSize - startPos;
        if (tailBytes > 0) {
            sdlSource->QueueAudioData(sdlSource->m_Buffer + startPos, tailBytes);
            sdlSource->m_TotalBytesPushed += tailBytes;
        }
        if (sdlSource->m_IsLooping && startPos > 0) {
            sdlSource->QueueAudioData(sdlSource->m_Buffer, startPos);
            sdlSource->m_TotalBytesPushed += startPos;
        }
        SDL_FlushAudioStream(sdlSource->m_Stream);
    }
}

int SdlSoundManager::GetPlayPosition(void *source)
{
    SdlSoundSource *sdlSource;
    Uint64 queued;
    Uint64 consumed;
    int queuedBytes;
    
    if (!ValidateSource(source)) return 0;
    
    sdlSource = (SdlSoundSource*)source;

    if (sdlSource->m_Stream) {
        queuedBytes = sdlSource->GetPendingSourceBytes();
        if (queuedBytes >= 0) {
            queued = (Uint64)queuedBytes;
            consumed = (sdlSource->m_TotalBytesPushed > queued) ? (sdlSource->m_TotalBytesPushed - queued) : 0;

            if (sdlSource->m_BufferSize > 0) {
                return (int)((sdlSource->m_PlayPos + consumed) % sdlSource->m_BufferSize);
            }
            return sdlSource->m_IsStreamed ? 0 : (int)consumed;
        }
    }

    return (int)sdlSource->m_PlayPos;
}

CKBOOL SdlSoundManager::IsPlaying(void *source)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return FALSE;
    
    sdlSource = (SdlSoundSource*)source;

    if (!sdlSource->m_IsPlaying || sdlSource->m_IsPaused) {
        return FALSE;
    }

    if (!sdlSource->m_Stream) {
        return FALSE;
    }

    const int queued = SDL_GetAudioStreamQueued(sdlSource->m_Stream);
    const int available = SDL_GetAudioStreamAvailable(sdlSource->m_Stream);
    const CKBOOL drained = (queued == 0 && available == 0);

    if (drained && !sdlSource->m_IsLooping) {
        // Sound has finished.
        if (sdlSource->m_IsBound) {
            SDL_UnbindAudioStream(sdlSource->m_Stream);
            sdlSource->m_IsBound = FALSE;
        }
        sdlSource->m_IsPlaying = FALSE;
        sdlSource->ForgetQueuedMixMatrix();
        return FALSE;
    }

    if (drained && sdlSource->m_IsLooping && !sdlSource->m_IsStreamed && sdlSource->m_Buffer && sdlSource->m_BufferSize > 0) {
        // Simple loop support: queue from the current cursor again.
        sdlSource->RefreshQueuedAudio();
    }

    return TRUE;
}

//-----------------------------------------------------------------------------
// PCM Buffer Information
//-----------------------------------------------------------------------------

CKERROR SdlSoundManager::SetWaveFormat(void *source, CKWaveFormat &wf)
{
    SdlSoundSource *sdlSource;
    SDL_AudioSpec spec;
    SDL_AudioSpec streamSpec;
    
    if (!ValidateSource(source)) return CKERR_INVALIDPARAMETER;
    
    sdlSource = (SdlSoundSource*)source;

    if (sdlSource->m_Stream) {
        SDL_ClearAudioStream(sdlSource->m_Stream);
        sdlSource->ForgetQueuedMixMatrix();
        sdlSource->m_TotalBytesPushed = 0;
        sdlSource->m_PlayPos = 0;
        if (sdlSource->m_IsStreamed) {
            sdlSource->m_WritePos = 0;
        }
    }

    memcpy(&sdlSource->m_Format, &wf, sizeof(CKWaveFormat));
    sdlSource->ResetMixMatrix();
    
    // Update stream format
    if (sdlSource->m_Stream) {
        spec.format = CKFormatToSDL(wf);
        spec.channels = wf.nChannels;
        spec.freq = wf.nSamplesPerSec;
        streamSpec = spec;
        if (streamSpec.channels == 1) {
            streamSpec.channels = 2;
        }
        SDL_SetAudioStreamFormat(sdlSource->m_Stream, &streamSpec, &streamSpec);
    }

    sdlSource->ApplySettings();
    sdlSource->Apply3DSettings(m_ListenerSettings);
    
    return CK_OK;
}

CKERROR SdlSoundManager::GetWaveFormat(void *source, CKWaveFormat &wf)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return CKERR_INVALIDPARAMETER;
    
    sdlSource = (SdlSoundSource*)source;
    memcpy(&wf, &sdlSource->m_Format, sizeof(CKWaveFormat));
    
    return CK_OK;
}

int SdlSoundManager::GetWaveSize(void *source)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return 0;
    
    sdlSource = (SdlSoundSource*)source;
    return (int)sdlSource->m_BufferSize;
}

//-----------------------------------------------------------------------------
// Buffer Access (Ring Buffer for Streaming)
//-----------------------------------------------------------------------------

CKERROR SdlSoundManager::Lock(void *source, CKDWORD dwWriteCursor, CKDWORD dwNumBytes, 
                             void **pvAudioPtr1, CKDWORD *dwAudioBytes1, 
                             void **pvAudioPtr2, CKDWORD *dwAudioBytes2, 
                             CK_WAVESOUND_LOCKMODE dwFlags)
{
    SdlSoundSource *sdlSource;
    CKDWORD bufferSize;
    CKDWORD startPos;
    CKDWORD audioBytes1;
    CKDWORD audioBytes2;
    CKDWORD requestedBytes;
    CKDWORD bytesToEnd;
    
    if (!ValidateSource(source) || !pvAudioPtr1 || !dwAudioBytes1) {
        return CKERR_INVALIDPARAMETER;
    }
    
    sdlSource = (SdlSoundSource*)source;
    
    if (!sdlSource->m_Buffer || sdlSource->m_BufferSize == 0) {
        return CKERR_INVALIDPARAMETER;
    }
    
    bufferSize = sdlSource->m_BufferSize;
    requestedBytes = (dwFlags & CK_WAVESOUND_LOCKENTIREBUFFER) ? bufferSize : dwNumBytes;

    if (requestedBytes > bufferSize) {
        return CKERR_INVALIDSIZE;
    }
    
    if (dwFlags & CK_WAVESOUND_LOCKENTIREBUFFER) {
        startPos = 0;
    } else if (dwFlags & CK_WAVESOUND_LOCKFROMWRITE) {
        startPos = sdlSource->m_WritePos;
    } else {
        startPos = dwWriteCursor % bufferSize;
    }

    // Calculate how many bytes until end of buffer
    bytesToEnd = bufferSize - startPos;
    audioBytes1 = min(requestedBytes, bytesToEnd);
    audioBytes2 = requestedBytes - audioBytes1;

    if (audioBytes2 > 0 && (!pvAudioPtr2 || !dwAudioBytes2)) {
        return CKERR_INVALIDPARAMETER;
    }

    *pvAudioPtr1 = sdlSource->m_Buffer + startPos;
    *dwAudioBytes1 = audioBytes1;

    if (pvAudioPtr2) {
        *pvAudioPtr2 = audioBytes2 ? sdlSource->m_Buffer : NULL;
    }
    if (dwAudioBytes2) {
        *dwAudioBytes2 = audioBytes2;
    }
    
    return CK_OK;
}

CKERROR SdlSoundManager::Unlock(void *source, void *pvAudioPtr1, CKDWORD dwNumBytes1, 
                                void *pvAudioPtr2, CKDWORD dwAudioBytes2)
{
    SdlSoundSource *sdlSource;
    CKDWORD totalBytes;
    
    if (!ValidateSource(source)) return CKERR_INVALIDPARAMETER;
    
    sdlSource = (SdlSoundSource*)source;
    totalBytes = dwNumBytes1 + dwAudioBytes2;
    
    // Update write position
    sdlSource->m_WritePos = (sdlSource->m_WritePos + totalBytes) % sdlSource->m_BufferSize;
    
    // For streamed sounds, feed data to SDL audio stream
    if (sdlSource->m_IsStreamed && sdlSource->m_Stream) {
        if (pvAudioPtr1 && dwNumBytes1 > 0) {
            sdlSource->QueueAudioData(pvAudioPtr1, dwNumBytes1);
            sdlSource->m_TotalBytesPushed += dwNumBytes1;
        }
        if (pvAudioPtr2 && dwAudioBytes2 > 0) {
            sdlSource->QueueAudioData(pvAudioPtr2, dwAudioBytes2);
            sdlSource->m_TotalBytesPushed += dwAudioBytes2;
        }
    }
    
    return CK_OK;
}

//-----------------------------------------------------------------------------
// Type Management
//-----------------------------------------------------------------------------

void SdlSoundManager::SetType(void *source, CK_WAVESOUND_TYPE type)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return;
    
    sdlSource = (SdlSoundSource*)source;
    sdlSource->m_Type = type;

    sdlSource->ApplySettings();
    sdlSource->Apply3DSettings(m_ListenerSettings);
}

CK_WAVESOUND_TYPE SdlSoundManager::GetType(void *source)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return (CK_WAVESOUND_TYPE)0;
    
    sdlSource = (SdlSoundSource*)source;
    return sdlSource->m_Type;
}

//-----------------------------------------------------------------------------
// Settings Management
//-----------------------------------------------------------------------------

void SdlSoundManager::UpdateSettings(void *source, CK_SOUNDMANAGER_CAPS settingsoptions, 
                                     CKWaveSoundSettings &settings, CKBOOL set)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return;
    
    sdlSource = (SdlSoundSource*)source;
    
    if (set) {
        // Set settings
        if (settingsoptions & CK_WAVESOUND_SETTINGS_GAIN) {
            sdlSource->m_Settings.m_Gain = settings.m_Gain;
        }
        
        if (settingsoptions & CK_WAVESOUND_SETTINGS_PITCH) {
            sdlSource->m_Settings.m_Pitch = settings.m_Pitch;
        }
        
        if (settingsoptions & CK_WAVESOUND_SETTINGS_PAN) {
            sdlSource->m_Settings.m_Pan = settings.m_Pan;
        }
        
        if (settingsoptions & CK_WAVESOUND_SETTINGS_PRIORITY) {
            sdlSource->m_Settings.m_Priority = settings.m_Priority;
        }
        
        if (settingsoptions & CK_WAVESOUND_SETTINGS_EQUALIZATION) {
            sdlSource->m_Settings.m_Eq = settings.m_Eq;
        }
        
        sdlSource->ApplySettings();
        sdlSource->Apply3DSettings(m_ListenerSettings);
    } else {
        // Get settings
        if (settingsoptions & CK_WAVESOUND_SETTINGS_GAIN) {
            settings.m_Gain = sdlSource->m_Settings.m_Gain;
        }
        
        if (settingsoptions & CK_WAVESOUND_SETTINGS_PITCH) {
            settings.m_Pitch = sdlSource->m_Settings.m_Pitch;
        }
        
        if (settingsoptions & CK_WAVESOUND_SETTINGS_PAN) {
            settings.m_Pan = sdlSource->m_Settings.m_Pan;
        }
        
        if (settingsoptions & CK_WAVESOUND_SETTINGS_PRIORITY) {
            settings.m_Priority = sdlSource->m_Settings.m_Priority;
        }
        
        if (settingsoptions & CK_WAVESOUND_SETTINGS_EQUALIZATION) {
            settings.m_Eq = sdlSource->m_Settings.m_Eq;
        }
    }
}

//-----------------------------------------------------------------------------
// 3D Settings Management
//-----------------------------------------------------------------------------

void SdlSoundManager::Update3DSettings(void *source, CK_SOUNDMANAGER_CAPS settingsoptions, 
                                       CKWaveSound3DSettings &settings, CKBOOL set)
{
    SdlSoundSource *sdlSource;
    
    if (!ValidateSource(source)) return;
    
    sdlSource = (SdlSoundSource*)source;
    
    if (set) {
        // Set 3D settings
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_CONE) {
            sdlSource->m_3DSettings.m_InAngle = settings.m_InAngle;
            sdlSource->m_3DSettings.m_OutAngle = settings.m_OutAngle;
            sdlSource->m_3DSettings.m_OutsideGain = settings.m_OutsideGain;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_MINMAXDISTANCE) {
            sdlSource->m_3DSettings.m_MinDistance = settings.m_MinDistance;
            sdlSource->m_3DSettings.m_MaxDistance = settings.m_MaxDistance;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_POSITION) {
            sdlSource->m_3DSettings.m_Position = settings.m_Position;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_VELOCITY) {
            sdlSource->m_3DSettings.m_Velocity = settings.m_Velocity;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_ORIENTATION) {
            sdlSource->m_3DSettings.m_OrientationDir = settings.m_OrientationDir;
            sdlSource->m_3DSettings.m_OrientationUp = settings.m_OrientationUp;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_HEADRELATIVE) {
            sdlSource->m_3DSettings.m_HeadRelative = settings.m_HeadRelative;
            sdlSource->m_3DSettings.m_MuteAfterMax = settings.m_MuteAfterMax;
        }
        
        // Apply 3D settings
        sdlSource->Apply3DSettings(m_ListenerSettings);
    } else {
        // Get 3D settings
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_CONE) {
            settings.m_InAngle = sdlSource->m_3DSettings.m_InAngle;
            settings.m_OutAngle = sdlSource->m_3DSettings.m_OutAngle;
            settings.m_OutsideGain = sdlSource->m_3DSettings.m_OutsideGain;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_MINMAXDISTANCE) {
            settings.m_MinDistance = sdlSource->m_3DSettings.m_MinDistance;
            settings.m_MaxDistance = sdlSource->m_3DSettings.m_MaxDistance;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_POSITION) {
            settings.m_Position = sdlSource->m_3DSettings.m_Position;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_VELOCITY) {
            settings.m_Velocity = sdlSource->m_3DSettings.m_Velocity;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_ORIENTATION) {
            settings.m_OrientationDir = sdlSource->m_3DSettings.m_OrientationDir;
            settings.m_OrientationUp = sdlSource->m_3DSettings.m_OrientationUp;
        }
        
        if (settingsoptions & CK_WAVESOUND_3DSETTINGS_HEADRELATIVE) {
            settings.m_HeadRelative = sdlSource->m_3DSettings.m_HeadRelative;
            settings.m_MuteAfterMax = sdlSource->m_3DSettings.m_MuteAfterMax;
        }
    }
}

//-----------------------------------------------------------------------------
// Listener Settings
//-----------------------------------------------------------------------------

void SdlSoundManager::UpdateListenerSettings(CK_SOUNDMANAGER_CAPS settingsoptions, 
                                             CKListenerSettings &settings, CKBOOL set)
{
    CKBOOL refresh3D;

    if (set) {
        refresh3D = FALSE;

        if (settingsoptions & CK_LISTENERSETTINGS_DISTANCE) {
            m_ListenerSettings.m_DistanceFactor = settings.m_DistanceFactor;
            refresh3D = TRUE;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_DOPPLER) {
            m_ListenerSettings.m_DopplerFactor = settings.m_DopplerFactor;
            refresh3D = TRUE;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_ROLLOFF) {
            m_ListenerSettings.m_RollOff = settings.m_RollOff;
            refresh3D = TRUE;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_GAIN) {
            m_ListenerSettings.m_GlobalGain = settings.m_GlobalGain;
            if (m_Device) {
                SDL_SetAudioDeviceGain(m_Device, settings.m_GlobalGain);
            }
        }
        if (settingsoptions & CK_LISTENERSETTINGS_PRIORITY) {
            m_ListenerSettings.m_PriorityBias = settings.m_PriorityBias;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_SOFTWARESOURCES) {
            m_ListenerSettings.m_SoftwareSources = settings.m_SoftwareSources;
        }

        if (refresh3D) {
            Refresh3DSources();
        }
    } else {
        if (settingsoptions & CK_LISTENERSETTINGS_DISTANCE) {
            settings.m_DistanceFactor = m_ListenerSettings.m_DistanceFactor;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_DOPPLER) {
            settings.m_DopplerFactor = m_ListenerSettings.m_DopplerFactor;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_ROLLOFF) {
            settings.m_RollOff = m_ListenerSettings.m_RollOff;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_GAIN) {
            settings.m_GlobalGain = m_ListenerSettings.m_GlobalGain;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_PRIORITY) {
            settings.m_PriorityBias = m_ListenerSettings.m_PriorityBias;
        }
        if (settingsoptions & CK_LISTENERSETTINGS_SOFTWARESOURCES) {
            settings.m_SoftwareSources = m_ListenerSettings.m_SoftwareSources;
        }
    }
}

//-----------------------------------------------------------------------------
// 3D Positioning
//-----------------------------------------------------------------------------

void SdlSoundManager::PositionSource(SdlSoundSource *source, CK3dEntity *ent, 
                                     const VxVector &position, const VxVector &direction, 
                                     VxVector &oldpos, float deltaTime)
{
    VxVector pos, vel, dir, up, baseUp;

    if (!source) return;
    
    // Calculate position
    pos = position;
    if (ent) {
        ent->Transform(&pos, &position);
    }
    
    // Calculate velocity
    if (deltaTime > 0.0f) {
        vel = (pos - oldpos) / (deltaTime * 0.001f);
    } else {
        vel.Set(0.0f, 0.0f, 0.0f);
    }

    // Calculate orientation
    dir = direction;
    up.Set(0.0f, 1.0f, 0.0f);
    if (ent) {
        ent->TransformVector(&dir, &direction);
        baseUp = up;
        ent->TransformVector(&up, &baseUp);
    }
    
    // Update 3D position in source settings
    source->m_3DSettings.m_Position = pos;
    source->m_3DSettings.m_Velocity = vel;
    source->m_3DSettings.m_OrientationDir = dir;
    source->m_3DSettings.m_OrientationUp = up;

    source->Apply3DSettings(m_ListenerSettings);
    
    oldpos = pos;
}

void SdlSoundManager::Refresh3DSources()
{
    CK_ID *it;
    CKWaveSound *ws;
    SoundMinion **itm;
    SdlSoundSource *source;

    for (it = m_SoundsPlaying.Begin(); it != m_SoundsPlaying.End(); ++it) {
        ws = (CKWaveSound*)m_Context->GetObject(*it);
        if (ws && ws->m_Source) {
            source = (SdlSoundSource*)ws->m_Source;
            source->Apply3DSettings(m_ListenerSettings);
        }
    }

    for (itm = m_Minions.Begin(); itm != m_Minions.End(); ++itm) {
        if (*itm && (*itm)->m_Source) {
            source = (SdlSoundSource*)(*itm)->m_Source;
            source->Apply3DSettings(m_ListenerSettings);
        }
    }
}

//-----------------------------------------------------------------------------
// Lifecycle Management
//-----------------------------------------------------------------------------

CKERROR SdlSoundManager::PostClearAll()
{
    CKERROR result;
    
    LockMutex();
    
    result = CKSoundManager::PostClearAll();
    m_SoundsPlaying.Clear();
    ReleaseMinions();
    RegisterAttribute();
    
    UnlockMutex();
    return result;
}

CKERROR SdlSoundManager::OnCKInit()
{
    SDL_AudioSpec spec;
    int soundsCount, i;
    CK_ID *soundIds;
    CKWaveSound *ws;
    
    if (m_Context->GetStartOptions() & CK_CONFIG_DISABLEDSOUND) {
        return CK_OK;
    }

    LockMutex();

    // Initialize SDL audio subsystem if needed
    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            if (m_Context->IsInInterfaceMode()) {
                m_Context->OutputToConsole("SDL audio initialization failed");
            }
            UnlockMutex();
            return CKERR_INVALIDOPERATION;
        }
    }
    
    // Set up audio format
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = SDL_DEFAULT_CHANNELS;
    spec.freq = SDL_DEFAULT_SAMPLE_RATE;

    // Open logical playback device
    m_Device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!m_Device) {
        if (m_Context->IsInInterfaceMode()) {
            m_Context->OutputToConsole("Failed to open SDL audio device");
        }
        UnlockMutex();
        return CKERR_INVALIDOPERATION;
    }

    SDL_SetAudioDeviceGain(m_Device, m_ListenerSettings.m_GlobalGain);
    
    RegisterAttribute();

    // Recreate existing sounds
    soundsCount = m_Context->GetObjectsCountByClassID(CKCID_WAVESOUND);
    if (soundsCount > 0) {
        soundIds = m_Context->GetObjectsListByClassID(CKCID_WAVESOUND);
        for (i = 0; i < soundsCount; ++i) {
            ws = (CKWaveSound*)m_Context->GetObject(soundIds[i]);
            if (ws) {
                ws->Recreate();
            }
        }
    }
    
    m_bInitialized = TRUE;
    UnlockMutex();
    return CK_OK;
}

CKERROR SdlSoundManager::OnCKEnd()
{
    if (m_Context->GetStartOptions() & CK_CONFIG_DISABLEDSOUND) {
        return CK_OK;
    }

    ShutdownAudio(TRUE);
    return CK_OK;
}

void SdlSoundManager::ShutdownAudio(CKBOOL releaseObjectSounds)
{
    LockMutex();
    
    if (releaseObjectSounds) {
        StopAllPlayingSounds();
    }
    ReleaseMinions();
    CleanupMixerResources();
    
    m_bInitialized = FALSE;
    
    UnlockMutex();
}

void SdlSoundManager::CleanupMixerResources()
{
    if (m_Device) {
        SDL_CloseAudioDevice(m_Device);
        m_Device = 0;
    }
}

void SdlSoundManager::StopAllPlayingSounds()
{
    int soundsCount, i;
    CK_ID *soundIds;
    CKWaveSound *ws;
    
    // Release existing sounds
    soundsCount = m_Context->GetObjectsCountByClassID(CKCID_WAVESOUND);
    if (soundsCount > 0) {
        soundIds = m_Context->GetObjectsListByClassID(CKCID_WAVESOUND);
        for (i = 0; i < soundsCount; ++i) {
            ws = (CKWaveSound*)m_Context->GetObject(soundIds[i]);
            if (ws) {
                ws->Release();
            }
        }
    }
}

CKERROR SdlSoundManager::PostProcess()
{
    float deltaTime;
    CK_ID *it;
    CKWaveSound *ws;
    SoundMinion **itm;
    SdlSoundSource *source;
    CK3dEntity *ent;
    CK3dEntity *listener;
    const VxMatrix *mat;
    const VxVector4 *pos;
    const VxVector4 *right;
    const VxVector4 *up;
    const VxVector4 *forward;
    VxVector oldListenerPosition;
    VxVector oldListenerVelocity;
    VxVector oldListenerRight;
    VxVector oldListenerUp;
    VxVector oldListenerForward;
    VxVector delta;
    
    if (!ValidateMixer()) return CK_OK;
    
    LockMutex();
    
    deltaTime = m_Context->GetTimeManager()->GetLastDeltaTime();
    oldListenerPosition = m_LastListenerPosition;
    oldListenerVelocity = m_LastListenerVelocity;
    oldListenerRight = m_LastListenerRight;
    oldListenerUp = m_LastListenerUp;
    oldListenerForward = m_LastListenerForward;

    listener = GetListener();
    if (listener) {
        mat = &listener->GetWorldMatrix();
        right = &(*mat)[0];
        up = &(*mat)[1];
        forward = &(*mat)[2];
        pos = &(*mat)[3];
        if (deltaTime > 0.0f) {
            m_LastListenerVelocity.Set((pos->x - m_LastListenerPosition.x) / (deltaTime * 0.001f),
                                       (pos->y - m_LastListenerPosition.y) / (deltaTime * 0.001f),
                                       (pos->z - m_LastListenerPosition.z) / (deltaTime * 0.001f));
        } else {
            m_LastListenerVelocity.Set(0.0f, 0.0f, 0.0f);
        }
        m_LastListenerRight.Set(right->x, right->y, right->z);
        m_LastListenerUp.Set(up->x, up->y, up->z);
        m_LastListenerForward.Set(forward->x, forward->y, forward->z);
        m_LastListenerPosition.Set(pos->x, pos->y, pos->z);
    } else {
        m_LastListenerPosition.Set(0.0f, 0.0f, 0.0f);
        m_LastListenerVelocity.Set(0.0f, 0.0f, 0.0f);
        m_LastListenerRight.Set(1.0f, 0.0f, 0.0f);
        m_LastListenerUp.Set(0.0f, 1.0f, 0.0f);
        m_LastListenerForward.Set(0.0f, 0.0f, 1.0f);
    }

    delta = m_LastListenerPosition - oldListenerPosition;
    if (delta.SquareMagnitude() > 0.000001f) {
        Refresh3DSources();
    } else {
        delta = m_LastListenerVelocity - oldListenerVelocity;
        if (delta.SquareMagnitude() > 0.000001f) {
            Refresh3DSources();
        } else {
            delta = m_LastListenerRight - oldListenerRight;
            if (delta.SquareMagnitude() > 0.000001f) {
                Refresh3DSources();
            } else {
                delta = m_LastListenerUp - oldListenerUp;
                if (delta.SquareMagnitude() > 0.000001f) {
                    Refresh3DSources();
                } else {
                    delta = m_LastListenerForward - oldListenerForward;
                    if (delta.SquareMagnitude() > 0.000001f) {
                        Refresh3DSources();
                    }
                }
            }
        }
    }

    // Update playing sounds
    for (it = m_SoundsPlaying.Begin(); it != m_SoundsPlaying.End();) {
        ws = (CKWaveSound*)m_Context->GetObject(*it);
        
        if (ws && ws->IsPlaying()) {
            // Handle file streaming
            if (ws->GetFileStreaming() && !(ws->GetState() & CK_WAVESOUND_STREAMFULLYLOADED)) {
                ws->WriteDataFromReader();
            }

            // Update fade
            ws->UpdateFade();

            // Update 3D position
            if (!(ws->GetType() & CK_WAVESOUND_BACKGROUND)) {
                ws->UpdatePosition(deltaTime);
            }

            ++it;
        } else {
            it = m_SoundsPlaying.Remove(it);
        }
    }

    // Update minions
    for (itm = m_Minions.Begin(); itm != m_Minions.End(); ++itm) {
        if (IsPlaying((*itm)->m_Source)) {
            source = (SdlSoundSource*)(*itm)->m_Source;
            
            if ((*itm)->m_Entity) {
                ent = (CK3dEntity*)m_Context->GetObject((*itm)->m_Entity);
                if (ent && source) {
                    PositionSource(source, ent, (*itm)->m_Position, 
                                  (*itm)->m_Direction, (*itm)->m_OldPosition, deltaTime);
                }
            }
        }
    }

    // Process minions (cleanup finished ones)
    ProcessMinions();
    
    UnlockMutex();
    return CK_OK;
}

CKERROR SdlSoundManager::OnCKReset()
{
    CK_ID *it;
    CKWaveSound *ws;
    
    if (!ValidateMixer()) return CK_OK;
    
    LockMutex();

    // Stop all playing sounds
    for (it = m_SoundsPlaying.Begin(); it != m_SoundsPlaying.End(); ++it) {
        ws = (CKWaveSound*)m_Context->GetObject(*it);
        if (ws && ws->m_Source) {
            ws->InternalStop();
        }
    }

    m_SoundsPlaying.Clear();
    ReleaseMinions();

    UnlockMutex();
    return CK_OK;
}

CKERROR SdlSoundManager::OnCKPause()
{
    CK_ID *it;
    CKWaveSound *ws;
    
    // Pause all currently playing sounds
    for (it = m_SoundsPlaying.Begin(); it != m_SoundsPlaying.End(); ++it) {
        ws = (CKWaveSound*)m_Context->GetObject(*it);
        if (ws) {
            ws->Pause();
        }
    }

    // Pause all minions
    PauseMinions();

    return CK_OK;
}

CKERROR SdlSoundManager::OnCKPlay()
{
    CK_ID *it;
    CKWaveSound *ws;
    
    // Resume all paused sounds
    for (it = m_SoundsPlaying.Begin(); it != m_SoundsPlaying.End(); ++it) {
        ws = (CKWaveSound*)m_Context->GetObject(*it);
        if (ws) {
            ws->Resume();
        }
    }

    // Resume all minions
    ResumeMinions();

    return CK_OK;
}

CKERROR SdlSoundManager::PreLaunchScene(CKScene *OldScene, CKScene *NewScene)
{
    CK_ID *it;
    CKWaveSound *ws;
    SoundMinion **itm;
    CKSceneObject *sceneObj;
    
    if (!NewScene) return CKERR_INVALIDPARAMETER;

    // Pause sounds that are not in the new scene
    for (it = m_SoundsPlaying.Begin(); it != m_SoundsPlaying.End(); ++it) {
        ws = (CKWaveSound*)m_Context->GetObject(*it);
        if (ws && !ws->IsInScene(NewScene)) {
            ws->Pause();
        }
    }

    // Stop minions whose models are not in the new scene
    for (itm = m_Minions.Begin(); itm != m_Minions.End();) {
        if (!*itm) {
            itm = m_Minions.Remove(itm);
            continue;
        }

        sceneObj = (CKSceneObject*)m_Context->GetObject((*itm)->m_OriginalSound);
        if (!sceneObj || !sceneObj->IsInScene(NewScene)) {
            // Stop and release the minion
            Stop(NULL, (*itm)->m_Source);
            ReleaseSource((*itm)->m_Source);
            delete *itm;
            itm = m_Minions.Remove(itm);
        } else {
            ++itm;
        }
    }

    return CK_OK;
}

CKERROR SdlSoundManager::SequenceToBeDeleted(CK_ID *objids, int count)
{
    CKERROR result;
    CK_ID *it;
    CKWaveSound *ws;
    SoundMinion **itm;
    CKObject *entity;
    
    if (!objids || count <= 0) return CKERR_INVALIDPARAMETER;

    // Call base class implementation
    result = CKSoundManager::SequenceDeleted(objids, count);

    // Stop and remove sounds that are being deleted
    for (it = m_SoundsPlaying.Begin(); it != m_SoundsPlaying.End();) {
        ws = (CKWaveSound*)m_Context->GetObject(*it);
        if (!ws || ws->IsToBeDeleted()) {
            if (ws) {
                ws->Stop();
            }
            it = m_SoundsPlaying.Remove(it);
        } else {
            ++it;
        }
    }

    // Clean up minions with deleted entities
    for (itm = m_Minions.Begin(); itm != m_Minions.End(); ++itm) {
        if (!*itm) continue;

        // Check if original sound is being deleted
        ws = (CKWaveSound*)m_Context->GetObject((*itm)->m_OriginalSound);
        if (ws && ws->IsToBeDeleted()) {
            (*itm)->m_OriginalSound = 0;
        }

        // Check if attached entity is being deleted
        entity = m_Context->GetObject((*itm)->m_Entity);
        if (!entity || entity->IsToBeDeleted()) {
            (*itm)->m_Entity = 0;
        }
    }

    return result;
}
