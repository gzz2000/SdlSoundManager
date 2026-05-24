#ifndef SDLSOUNDMANAGER_H
#define SDLSOUNDMANAGER_H

#include "CKAll.h"

#include <SDL3/SDL.h>

// Constants for better maintainability
#define SDL_DEFAULT_SAMPLE_RATE     22050
#define SDL_DEFAULT_CHANNELS        2
#define SDL_DEFAULT_BITS_PER_SAMPLE 16

// Forward declaration of internal source structure
struct SdlSoundSource;

/**
 * @brief SDL3-based Sound Manager implementation
 * 
 * This class implements the CKSoundManager interface using SDL3 audio streams.
 * SDL3_mixer is intentionally not used.
 */
class SdlSoundManager : public CKSoundManager
{
    friend class CKWaveSound;

public:
    // Constructor/Destructor
    SdlSoundManager(CKContext *Context);
    virtual ~SdlSoundManager();

    // Get the caps of the sound manager
    virtual CK_SOUNDMANAGER_CAPS GetCaps();

    // Creation and management
    virtual void *CreateSource(CK_WAVESOUND_TYPE flags, CKWaveFormat *wf, CKDWORD bytes, CKBOOL streamed);
    virtual void *DuplicateSource(void *source);
    virtual void ReleaseSource(void *source);

    // Playback control
    virtual void Play(CKWaveSound *ws, void *source, CKBOOL loop);
    virtual void Pause(CKWaveSound *ws, void *source);
    virtual void SetPlayPosition(void *source, int pos);
    virtual int GetPlayPosition(void *source);
    virtual CKBOOL IsPlaying(void *source);

    // PCM Buffer Information
    virtual CKERROR SetWaveFormat(void *source, CKWaveFormat &wf);
    virtual CKERROR GetWaveFormat(void *source, CKWaveFormat &wf);
    virtual int GetWaveSize(void *source);

    // Buffer access (ring buffer for streaming)
    virtual CKERROR Lock(void *source, CKDWORD dwWriteCursor, CKDWORD dwNumBytes, 
                        void **pvAudioPtr1, CKDWORD *dwAudioBytes1, 
                        void **pvAudioPtr2, CKDWORD *dwAudioBytes2, 
                        CK_WAVESOUND_LOCKMODE dwFlags);
    virtual CKERROR Unlock(void *source, void *pvAudioPtr1, CKDWORD dwNumBytes1, 
                          void *pvAudioPtr2, CKDWORD dwAudioBytes2);

    // 2D/3D Members Functions
    virtual void SetType(void *source, CK_WAVESOUND_TYPE type);
    virtual CK_WAVESOUND_TYPE GetType(void *source);

    // 2D/3D Settings
    virtual void UpdateSettings(void *source, CK_SOUNDMANAGER_CAPS settingsoptions, 
                               CKWaveSoundSettings &settings, CKBOOL set /* = TRUE */);

    // 3D Settings
    virtual void Update3DSettings(void *source, CK_SOUNDMANAGER_CAPS settingsoptions, 
                                 CKWaveSound3DSettings &settings, CKBOOL set /* = TRUE */);

    // Listener settings
    virtual void UpdateListenerSettings(CK_SOUNDMANAGER_CAPS settingsoptions, 
                                       CKListenerSettings &settings, CKBOOL set /* = TRUE */);

    // Lifecycle management
    virtual CKERROR OnCKInit();
    virtual CKERROR OnCKEnd();
    virtual CKERROR OnCKReset();
    virtual CKERROR OnCKPause();
    virtual CKERROR OnCKPlay();
    virtual CKERROR PostClearAll();
    virtual CKERROR PostProcess();
    virtual CKERROR PreLaunchScene(CKScene *OldScene, CKScene *NewScene);
    virtual CKERROR SequenceToBeDeleted(CK_ID *objids, int count);

    // Status
    virtual CKBOOL IsInitialized();

    // Manager Function Mask
    virtual CKDWORD GetValidFunctionsMask()
    {
        return CKSoundManager::GetValidFunctionsMask() |
               CKMANAGER_FUNC_OnCKPlay |
               CKMANAGER_FUNC_PostClearAll |
               CKMANAGER_FUNC_OnCKInit |
               CKMANAGER_FUNC_OnCKEnd |
               CKMANAGER_FUNC_OnCKReset |
               CKMANAGER_FUNC_OnCKPause |
               CKMANAGER_FUNC_PostProcess |
               CKMANAGER_FUNC_OnSequenceToBeDeleted |
               CKMANAGER_FUNC_PreLaunchScene;
    }

protected:
    // Internal helper methods
    void InternalPause(void *source);
    void InternalPlay(void *source, CKBOOL loop /* = FALSE */);
    
    // Source positioning for 3D audio
    void PositionSource(SdlSoundSource *source, CK3dEntity *ent, 
                       const VxVector &position, const VxVector &direction, 
                       VxVector &oldpos, float deltaTime);
    void Refresh3DSources();
    
    // Resource validation
    CKBOOL ValidateSource(void *source) const;
    CKBOOL ValidateMixer() const;
    
    // Cleanup helpers
    void CleanupMixerResources();
    void ShutdownAudio(CKBOOL releaseObjectSounds);
    void StopAllPlayingSounds();

    // Audio format conversion helpers
    static float DbToLinear(float db);
    static float LinearToDb(float linear);

public:
    // Made public for SdlSoundSource access
    static SDL_AudioFormat CKFormatToSDL(const CKWaveFormat &wf);

    // Grant SdlSoundSource access to private members
    friend struct SdlSoundSource;

private:
    // SDL audio device (logical playback device)
    SDL_AudioDeviceID m_Device;
    
    // Internal state
    CKBOOL m_bInitialized;
    VxVector m_LastListenerPosition;
    VxVector m_LastListenerVelocity;
    VxVector m_LastListenerRight;
    VxVector m_LastListenerUp;
    VxVector m_LastListenerForward;
    
    // Listener settings (cached)
    CKListenerSettings m_ListenerSettings;
    
    // Currently playing sounds
    XObjectArray m_SoundsPlaying;
    
    // Thread safety
    SDL_Mutex *m_Mutex;
    
    // Helper methods for thread safety
    void LockMutex() const;
    void UnlockMutex() const;

    // Prevent copy construction and assignment
    SdlSoundManager(const SdlSoundManager&);
    SdlSoundManager& operator=(const SdlSoundManager&);
};

/**
 * @brief Internal sound source structure
 * 
 * Represents a single sound source with its buffer, track, and settings.
 */
struct SdlSoundSource
{
    friend class SdlSoundManager;
    // SDL Audio stream for streaming
    SDL_AudioStream *m_Stream;

    // Bookkeeping for estimating playback position
    Uint64 m_TotalBytesPushed;

    // Bound to the device for playback
    CKBOOL m_IsBound;
    
    // Ring buffer for PCM data
    CKBYTE *m_Buffer;
    CKDWORD m_BufferSize;
    
    // Buffer positions
    CKDWORD m_WritePos;
    CKDWORD m_PlayPos;
    
    // Wave format
    CKWaveFormat m_Format;
    
    // Sound type (2D/3D)
    CK_WAVESOUND_TYPE m_Type;
    
    // Current settings
    CKWaveSoundSettings m_Settings;
    CKWaveSound3DSettings m_3DSettings;
    
    // Playback state
    CKBOOL m_IsPlaying;
    CKBOOL m_IsPaused;
    CKBOOL m_IsLooping;
    CKBOOL m_IsStreamed;
    
    // Reference to parent manager
    SdlSoundManager *m_Manager;

    // Software stereo panning matrix
    float m_LeftToLeftGain;
    float m_LeftToRightGain;
    float m_RightToLeftGain;
    float m_RightToRightGain;
    
    SdlSoundSource();
    ~SdlSoundSource();
    
    void Reset();
    CKBOOL Initialize(SdlSoundManager *manager, CK_WAVESOUND_TYPE type, 
                     const CKWaveFormat *wf, CKDWORD bytes, CKBOOL streamed);
    void Cleanup();
    void ResetMixMatrix();
    
    // Update track settings from current state
    void ApplySettings();
    void Apply3DSettings(const CKListenerSettings &listener);
    void QueueAudioData(const void *data, CKDWORD bytes);
    void RefreshQueuedAudio();
    int GetQueuedSourceBytes() const;
};

// Factory function
CKERROR CreateSdlSoundManager(CKContext *context);

#endif // SDLSOUNDMANAGER_H
