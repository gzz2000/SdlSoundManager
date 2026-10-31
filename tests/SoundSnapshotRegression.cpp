#include "SdlSoundManager.h"
#undef min
#undef max
#include <cstdio>
#include <cstdlib>
#include <cmath>
static int checks;
static void Require(bool ok, const char *message) {
    ++checks; if (!ok) { std::fprintf(stderr, "FAIL %s (%s)\n", message, SDL_GetError()); std::exit(1); }
}
static void Restore(CKContext *context, void *snapshot) {
    void *prepared = nullptr;
    Require(SDL_SoundPrepareRestore(context, snapshot, &prepared)==CK_OK,"prepare sound restore");
    Require(SDL_SoundCommitRestore(context, prepared)==CK_OK,"commit sound restore");
    Require(SDL_SoundCommitRestore(context, prepared)!=CK_OK,"reject double commit");
    SDL_SoundDestroyRestore(prepared);
}
int main() {
#ifdef _WIN32
    _putenv_s("SDL_AUDIO_DRIVER", "dummy");
#else
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);
#endif
    Require(CKStartUp()==CK_OK,"CK startup");
    CKContext *context = nullptr;
    Require(CKCreateContext(&context,nullptr,0,0)==CK_OK,"context");
    auto *manager = new SdlSoundManager(context);
    Require(manager->OnCKInit()==CK_OK,"audio init");
    context->GetTimeManager()->SetFixedDeltaTime(10.0f);
    context->GetTimeManager()->SetLastDeltaTime(10.0f);
    auto *wave = CKWaveSound::Cast(context->CreateObject(CKCID_WAVESOUND,"test PCM"));
    Require(wave!=nullptr,"wave");
    CKWaveFormat format{};format.wFormatTag=1;format.nChannels=1;format.nSamplesPerSec=1000;
    format.nAvgBytesPerSec=2000;format.nBlockAlign=2;format.wBitsPerSample=16;
    Require(wave->Create(CK_WAVESOUND_BACKGROUND,&format,2000)==CK_OK,"one second PCM");
    wave->Play(200.0f,0.75f);
    auto *source = static_cast<SdlSoundSource *>(wave->m_Source);
    Require(wave->IsPlaying(),"playing");
    SDL_Delay(60);
    Require(source->m_SimulationFrame==0.0 && wave->GetPlayPosition()==0,"wall time cannot advance logical sound");
    for(int i=0;i<7;++i) { manager->PreProcess();manager->PostProcess(); }
    Require(wave->GetPlayPosition()==140,"seven simulation steps");
    const float fade=wave->m_CurrentTime;
    void *saved = nullptr;
    Require(SDL_SoundCaptureSnapshot(context,&saved)==CK_OK,"capture fade and position");
    Require(source->m_SimulationFrame==70.0 && wave->m_CurrentTime==fade,"capture is immutable");
    void *handle = wave->PlayMinion(TRUE);
    Require(handle!=nullptr,"transient minion");
    void *withMinion=nullptr;
    Require(SDL_SoundCaptureSnapshot(context,&withMinion)==CK_OK,"capture minion");
    for(int i=0;i<100;++i) { manager->PreProcess();manager->PostProcess(); }
    Require(!wave->IsPlaying(),"logical end independent of device");
    for(int cycle=0;cycle<20;++cycle) {
        Restore(context,withMinion);
        Require(manager->IsPlaying(handle),"expired minion handle restored");
        Require(wave->m_Source==source && source->m_SimulationFrame==70.0,"source identity and fractional playhead restored");
        Require(wave->m_CurrentTime==fade,"fade restored");
        for(int i=0;i<100;++i) { manager->PreProcess();manager->PostProcess(); }
    }
    Restore(context,saved);
    wave->Pause();manager->PreProcess();
    Require(wave->GetPlayPosition()==140,"pause freezes logical audio");
    wave->Resume();manager->PreProcess();
    Require(wave->GetPlayPosition()==160,"resume continues logical audio");
    wave->SetLoopMode(TRUE);wave->SetPitch(1.5f);
    context->GetTimeManager()->SetLastDeltaTime(1.0f);
    manager->PreProcess();
    Require(source->m_SimulationFrame==81.5,"fractional pitched samples retained");
    void *overwrite=nullptr;
    Require(SDL_SoundCaptureSnapshot(context,&overwrite)==CK_OK,"overwrite snapshot state");
    for(int i=0;i<4;++i) {
        for(int j=0;j<1000;++j)manager->PreProcess();
        Require(wave->IsPlaying(),"loop survives logical wrap");
        Restore(context,overwrite);
        Require(source->m_SimulationFrame==81.5 && wave->GetLoopMode(),"repeated fractional/loop restore");
    }
    auto *stream = static_cast<SdlSoundSource *>(manager->CreateSource(CK_WAVESOUND_BACKGROUND,&format,2000,TRUE));
    Require(stream!=nullptr,"stream source");void *unsupported=reinterpret_cast<void *>(1);
    Require(SDL_SoundCaptureSnapshot(context,&unsupported)!=CK_OK && unsupported==nullptr,"streaming explicitly rejected");
    manager->ReleaseSource(stream);
    Restore(context,saved);
    // All resources and live state survive a deliberately abandoned preparation.
    void *prepared=nullptr;
    Require(SDL_SoundPrepareRestore(context,saved,&prepared)==CK_OK,"prepare then abandon");
    SDL_SoundDestroyRestore(prepared);
    Require(source->m_SimulationFrame==70.0 && wave->IsPlaying(),"abandon does not mutate playback");
    // Recreating the source after saving must not recycle a retained handle.
    Require(wave->Create(CK_WAVESOUND_BACKGROUND,&format,1000)==CK_OK,"recreate future source");
    Require(wave->m_Source!=source,"saved source lifetime is retained");
    Restore(context,saved);
    Require(wave->m_Source==source && source->m_BufferSize==2000 &&
        source->m_SimulationFrame==70.0,"restore replaced source and PCM size");
    handle=wave->PlayMinion(TRUE);
    void *retainMinion=nullptr;
    Require(SDL_SoundCaptureSnapshot(context,&retainMinion)==CK_OK,"retain minion during unrelated deletion");
    CKObject *unrelated=context->CreateObject(CKCID_OBJECT,"unrelated temporary object");
    CK_ID deleted=unrelated->GetID();
    manager->SequenceToBeDeleted(&deleted,1);
    Require(manager->IsPlaying(handle),"unrelated deletion must not stop minions");
    const float beforeFade=wave->m_CurrentTime;
    manager->PostProcess();
    Require(wave->m_CurrentTime>beforeFade,"unrelated deletion must not drop fade processing");
    context->DestroyObject(unrelated);
    manager->SequenceDeleted(&deleted,1);
    Require(manager->IsPlaying(handle),"post-deletion callback must not stop unrelated minions");
    const float afterDeleteFade=wave->m_CurrentTime;
    manager->PostProcess();
    Require(wave->m_CurrentTime>afterDeleteFade,"post-deletion callback preserves fades");
    SDL_SoundDestroySnapshot(retainMinion);
    manager->SetWaveFormat(source,format);
    Require(source->m_SimulationFrame==0.0,"new format resets logical cursor");
    void *outlivesContext=nullptr;
    Require(SDL_SoundCaptureSnapshot(context,&outlivesContext)==CK_OK,"retain source beyond context teardown");
    manager->OnCKReset();
    Require(SDL_SoundPrepareRestore(context,saved,&prepared)!=CK_OK,"old epoch rejected");
    SDL_SoundDestroySnapshot(saved);SDL_SoundDestroySnapshot(withMinion);SDL_SoundDestroySnapshot(overwrite);
    Require(CKCloseContext(context)==CK_OK,"context close");
    SDL_SoundDestroySnapshot(outlivesContext);
    Require(CKShutdown()==CK_OK,"shutdown");
    std::printf("PASS %d checks\n",checks);
}
