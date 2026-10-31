#include "SdlSoundManager.h"
#undef min
#undef max
#include <cmath>
#include <utility>

bool SdlSoundManager::UsesSimulationClock() const {
    return m_Context->GetTimeManager()->GetFixedDeltaTime() > 0.0f;
}

CKDWORD SdlSoundSource::SimulationBytePosition() const {
    if (!m_Format.nBlockAlign || !m_BufferSize) return 0;
    const double frames = m_BufferSize / m_Format.nBlockAlign;
    if (!std::isfinite(m_SimulationFrame) || m_SimulationFrame <= 0.0) return 0;
    // Completed buffers wrap, as the historical GetPlayPosition API does.
    return CKDWORD(std::floor(std::fmod(m_SimulationFrame, frames))) * m_Format.nBlockAlign;
}

void SdlSoundSource::Silence() {
    if (m_Stream && m_IsBound) SDL_UnbindAudioStream(m_Stream);
    m_IsBound = FALSE;
}

void SdlSoundSource::AdvanceSimulation(float milliseconds) {
    if (!m_IsPlaying || m_IsPaused || m_IsStreamed || !m_Format.nBlockAlign ||
        !std::isfinite(milliseconds) || milliseconds <= 0.0f) return;
    const double end = m_BufferSize / m_Format.nBlockAlign;
    if (end <= 0.0) return;
    m_SimulationFrame += double(milliseconds) * double(m_Format.nSamplesPerSec) * double(m_EffectivePitch) / 1000.0;
    if (m_SimulationFrame >= end) {
        if (m_IsLooping) m_SimulationFrame = std::fmod(m_SimulationFrame, end);
        else {
            m_SimulationFrame = end;
            m_IsPlaying = FALSE;
            Silence();
        }
    }
}

CKERROR SdlSoundManager::PreProcess() {
    if (!UsesSimulationClock()) return CK_OK;
    const float dt = m_Context->GetTimeManager()->GetLastDeltaTime();
    for (const auto &source : m_Sources) source.first->AdvanceSimulation(dt);
    return CK_OK;
}

void SdlSoundSource::Swap(SdlSoundSource &other) {
#define FIELD(x) std::swap(x, other.x)
    FIELD(m_Stream); FIELD(m_TotalBytesPushed); FIELD(m_IsBound);
    FIELD(m_Buffer); FIELD(m_BufferSize); FIELD(m_MixBuffer); FIELD(m_MixBufferSize);
    FIELD(m_WritePos); FIELD(m_PlayPos); FIELD(m_SimulationFrame); FIELD(m_EffectivePitch);
    FIELD(m_Format); FIELD(m_Type); FIELD(m_Settings); FIELD(m_3DSettings);
    FIELD(m_IsPlaying); FIELD(m_IsPaused); FIELD(m_IsLooping); FIELD(m_IsStreamed);
    FIELD(m_Manager); FIELD(m_LeftToLeftGain); FIELD(m_LeftToRightGain);
    FIELD(m_RightToLeftGain); FIELD(m_RightToRightGain);
    FIELD(m_QueuedLeftToLeftGain); FIELD(m_QueuedLeftToRightGain);
    FIELD(m_QueuedRightToLeftGain); FIELD(m_QueuedRightToRightGain); FIELD(m_HasQueuedMixMatrix);
#undef FIELD
}
