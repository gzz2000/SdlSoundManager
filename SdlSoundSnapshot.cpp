#include "SdlSoundManager.h"
#include "CKObjectLifetime.h"
#undef min
#undef max
#include <vector>
#include <string>
#include <cstring>
#include <new>
#include <utility>

// Only authored/runtime values are copied. SDL queues, device handles and
// scratch mixing buffers are regenerated; retained source objects preserve the
// handles returned by CKWaveSound::PlayMinion across a rewind.
#define SOURCE_FIELDS(X) \
    X(m_WritePos) X(m_PlayPos) X(m_SimulationFrame) X(m_EffectivePitch) \
    X(m_Format) X(m_Type) X(m_Settings) X(m_3DSettings) \
    X(m_IsPlaying) X(m_IsPaused) X(m_IsLooping) X(m_IsStreamed) \
    X(m_LeftToLeftGain) X(m_LeftToRightGain) X(m_RightToLeftGain) X(m_RightToRightGain)
#define WAVE_FIELDS(X) \
    X(m_State) X(m_AttachedObject) X(m_OldPosition) X(m_Position) X(m_Direction) \
    X(m_FinalGain) X(m_FadeTime) X(m_CurrentTime) X(m_BufferPos) X(m_BufferSize) \
    X(m_DataRead) X(m_DataToRead) X(m_DataPlayed) X(m_OldCursorPos) X(m_Duration) \
    X(m_WaveFormat) X(m_2DSetting) X(m_3DSetting)
#define MANAGER_FIELDS(X) \
    X(m_LastListenerPosition) X(m_LastListenerVelocity) X(m_LastListenerRight) \
    X(m_LastListenerUp) X(m_LastListenerForward) X(m_ListenerSettings) \
    X(m_ListenerEntity) X(m_OldListenerPos) X(m_BufferSize)

struct SdlSourceState {
    std::shared_ptr<SdlSoundSource> identity;
    std::vector<CKBYTE> pcm;
    float effectiveGain;
#define DECLARE(x) decltype(SdlSoundSource::x) x;
    SOURCE_FIELDS(DECLARE)
#undef DECLARE
    explicit SdlSourceState(const std::shared_ptr<SdlSoundSource> &source) : identity(source) {
#define COPY(x) x = source->x;
        SOURCE_FIELDS(COPY)
#undef COPY
        pcm.assign(source->m_Buffer, source->m_Buffer + source->m_BufferSize);
        effectiveGain = SDL_GetAudioStreamGain(source->m_Stream);
    }
    std::unique_ptr<SdlSoundSource> Prepare(SdlSoundManager *manager) const {
        auto result = std::make_unique<SdlSoundSource>();
        if (!result->Initialize(manager, m_Type, &m_Format, CKDWORD(pcm.size()), FALSE)) return {};
        std::memcpy(result->m_Buffer, pcm.data(), pcm.size());
#define COPY(x) result->x = x;
        SOURCE_FIELDS(COPY)
#undef COPY
        // Queue preparation must not bind to the live device or advance logic.
        SDL_SetAudioStreamFrequencyRatio(result->m_Stream, m_EffectivePitch);
        SDL_SetAudioStreamGain(result->m_Stream, effectiveGain);
        result->m_PlayPos = result->SimulationBytePosition();
        result->RefreshQueuedAudio();
        return result;
    }
};

struct SdlWaveState {
    CK_ID id;
    std::uint64_t lifetime;
    SdlSoundSource *source;
    CKSoundReader *reader;
    std::string file;
#define DECLARE(x) decltype(CKWaveSound::x) x;
    WAVE_FIELDS(DECLARE)
#undef DECLARE
    explicit SdlWaveState(CKContext *context, CKWaveSound *wave)
        : id(wave->GetID()), lifetime(CKGetObjectLifetimeToken(context, id)),
          source(static_cast<SdlSoundSource *>(wave->m_Source)), reader(wave->m_SoundReader),
          file(wave->GetSoundFileName() ? wave->GetSoundFileName() : "") {
#define COPY(x) x = wave->x;
        WAVE_FIELDS(COPY)
#undef COPY
    }
    bool Validate(CKContext *context) const {
        auto *wave = CKWaveSound::Cast(context->GetObject(id));
        return wave && lifetime && CKGetObjectLifetimeToken(context, id) == lifetime &&
            wave->m_SoundReader == reader && file == (wave->GetSoundFileName() ? wave->GetSoundFileName() : "");
    }
    void Restore(CKWaveSound *wave) const {
#define COPY(x) wave->x = x;
        WAVE_FIELDS(COPY)
#undef COPY
        wave->m_Source = source;
    }
};

struct SdlSoundSnapshot {
    CKContext *context;
    SdlSoundManager *manager;
    unsigned long long epoch;
    std::vector<SdlSourceState> sources;
    std::vector<SdlWaveState> waves;
    std::vector<SoundMinion> minions;
    XObjectArray playing;
#define DECLARE(x) decltype(SdlSoundManager::x) x;
    MANAGER_FIELDS(DECLARE)
#undef DECLARE
    bool Validate(CKContext *c) const {
        if (c != context || c->GetManagerByGuid(SOUND_MANAGER_GUID) != manager ||
            manager->m_SnapshotEpoch != epoch || !manager->UsesSimulationClock()) return false;
        for (const auto &wave : waves) if (!wave.Validate(c)) return false;
        if (c->GetObjectsCountByClassID(CKCID_WAVESOUND) != int(waves.size())) return false;
        return true;
    }
    static std::unique_ptr<SdlSoundSnapshot> Capture(CKContext *c) {
        auto *m = c ? dynamic_cast<SdlSoundManager *>(c->GetManagerByGuid(SOUND_MANAGER_GUID)) : nullptr;
        if (!m || !m->UsesSimulationClock()) return {};
        auto result = std::make_unique<SdlSoundSnapshot>();
        result->context = c; result->manager = m; result->epoch = m->m_SnapshotEpoch;
#define COPY(x) result->x = m->x;
        MANAGER_FIELDS(COPY)
#undef COPY
        for (const auto &entry : m->m_Sources) {
            // A decoder/ring-buffer checkpoint needs its own state API. Never
            // silently claim it is restored by seeking to a rounded timestamp.
            if (entry.first->m_IsStreamed) {
                c->OutputToConsole("Sound snapshot: streaming decoder state is not supported yet.");
                return {};
            }
            result->sources.emplace_back(entry.second);
        }
        auto *ids = c->GetObjectsListByClassID(CKCID_WAVESOUND);
        int count = c->GetObjectsCountByClassID(CKCID_WAVESOUND);
        for (int i = 0; i < count; ++i) {
            auto *wave = CKWaveSound::Cast(c->GetObject(ids[i]));
            if (!wave || wave->m_SoundManager != m ||
                (wave->m_Source && !m->m_Sources.count(static_cast<SdlSoundSource *>(wave->m_Source)))) return {};
            result->waves.emplace_back(c, wave);
        }
        for (auto it = m->m_Minions.Begin(); it != m->m_Minions.End(); ++it) {
            if (!*it || !m->m_Sources.count(static_cast<SdlSoundSource *>((*it)->m_Source)) ||
                ((*it)->m_OriginalSource && !m->m_Sources.count(static_cast<SdlSoundSource *>((*it)->m_OriginalSource)))) return {};
            result->minions.push_back(**it);
        }
        result->playing = m->m_SoundsPlaying;
        return result;
    }
};

struct SdlSoundRestore {
    const SdlSoundSnapshot *snapshot;
    std::vector<std::unique_ptr<SdlSoundSource>> prepared;
    std::map<SdlSoundSource *, std::shared_ptr<SdlSoundSource>> sources;
    std::vector<std::unique_ptr<SoundMinion>> ownedMinions;
    XArray<SoundMinion *> minions;
    XObjectArray playing;
    bool committed = false;
    static std::unique_ptr<SdlSoundRestore> Prepare(CKContext *c, const SdlSoundSnapshot *s) {
        if (!s || !s->Validate(c)) return {};
        auto result = std::make_unique<SdlSoundRestore>();
        result->snapshot = s;
        for (const auto &source : s->sources) {
            auto replacement = source.Prepare(s->manager);
            if (!replacement) return {};
            result->prepared.push_back(std::move(replacement));
            result->sources.emplace(source.identity.get(), source.identity);
        }
        for (const auto &minion : s->minions) {
            auto value = std::make_unique<SoundMinion>(minion);
            result->minions.PushBack(value.get());
            result->ownedMinions.push_back(std::move(value));
        }
        result->playing = s->playing;
        return result;
    }
    int Commit(CKContext *c) {
        if (committed || !snapshot->Validate(c)) return CKERR_INVALIDPARAMETER;
        auto *m = snapshot->manager;
        // Allocation and source creation are finished before touching live audio.
        // SDL binding below affects presentation only, not the logical playhead.
        for (const auto &entry : m->m_Sources) entry.first->Silence();
        m->ReleaseMinions();
        for (std::size_t i = 0; i < snapshot->sources.size(); ++i)
            snapshot->sources[i].identity->Swap(*prepared[i]);
        m->m_Sources.swap(sources);
        for (const auto &wave : snapshot->waves) wave.Restore(CKWaveSound::Cast(c->GetObject(wave.id)));
#define COPY(x) m->x = snapshot->x;
        MANAGER_FIELDS(COPY)
#undef COPY
        SDL_SetAudioDeviceGain(m->m_Device, m->m_ListenerSettings.m_GlobalGain);
        m->m_SoundsPlaying.Swap(playing);
        m->m_Minions.Swap(minions);
        for (auto &minion : ownedMinions) minion.release();
        for (const auto &entry : m->m_Sources) {
            auto *source = entry.first;
            if (source->m_IsPlaying && !source->m_IsPaused)
                source->m_IsBound = SDL_BindAudioStream(m->m_Device, source->m_Stream);
        }
        committed = true;
        return CK_OK;
    }
};

extern "C" DLL_EXPORT int SDL_SoundCaptureSnapshot(CKContext *c, void **out) {
    if (!out) return CKERR_INVALIDPARAMETER;
    *out = nullptr;
    try {
        auto snapshot = SdlSoundSnapshot::Capture(c);
        if (!snapshot) return CKERR_INVALIDPARAMETER;
        *out = snapshot.release(); return CK_OK;
    } catch (const std::bad_alloc &) { return CKERR_OUTOFMEMORY; }
}
extern "C" DLL_EXPORT int SDL_SoundPrepareRestore(CKContext *c, void *value, void **out) {
    if (!out) return CKERR_INVALIDPARAMETER;
    *out = nullptr;
    try {
        auto prepared = SdlSoundRestore::Prepare(c, static_cast<SdlSoundSnapshot *>(value));
        if (!prepared) return CKERR_INVALIDPARAMETER;
        *out = prepared.release(); return CK_OK;
    } catch (const std::bad_alloc &) { return CKERR_OUTOFMEMORY; }
}
extern "C" DLL_EXPORT int SDL_SoundCommitRestore(CKContext *c, void *value) {
    return value ? static_cast<SdlSoundRestore *>(value)->Commit(c) : CKERR_INVALIDPARAMETER;
}
extern "C" DLL_EXPORT void SDL_SoundDestroySnapshot(void *value) { delete static_cast<SdlSoundSnapshot *>(value); }
extern "C" DLL_EXPORT void SDL_SoundDestroyRestore(void *value) { delete static_cast<SdlSoundRestore *>(value); }
