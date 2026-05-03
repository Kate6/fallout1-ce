#include "audio_engine.h"

#include <string.h>

#include <mutex>

#include <SDL.h>

namespace fallout {

#define AUDIO_ENGINE_SOUND_BUFFERS 8

struct AudioEngineSoundBuffer {
    bool active;
    unsigned int size;
    int bitsPerSample;
    int channels;
    int rate;
    void* data;
    int volume;
    bool playing;
    bool looping;
    unsigned int pos;
    unsigned int writePos;
    SDL_AudioStream* stream;
    std::recursive_mutex mutex;
};

extern bool GNW95_isActive;

static bool soundBufferIsValid(int soundBufferIndex);
static void audioEngineMixin(void* userData, Uint8* stream, int length);

static SDL_AudioSpec gAudioEngineSpec;
static SDL_AudioDeviceID gAudioEngineDeviceId = -1;
static AudioEngineSoundBuffer gAudioEngineSoundBuffers[AUDIO_ENGINE_SOUND_BUFFERS];

static bool audioEngineIsInitialized()
{
    return gAudioEngineDeviceId != -1;
}

static bool soundBufferIsValid(int soundBufferIndex)
{
    return soundBufferIndex >= 0 && soundBufferIndex < AUDIO_ENGINE_SOUND_BUFFERS;
}

static void audioEngineMixin(void* userData, Uint8* stream, int length)
{
    memset(stream, gAudioEngineSpec.silence, length);

    if (!GNW95_isActive) {
        return;
    }

    for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
        AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[index]);
        std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

        if (soundBuffer->active && soundBuffer->playing) {
            int srcFrameSize = soundBuffer->bitsPerSample / 8 * soundBuffer->channels;
            int srcRate = soundBuffer->rate;
            int dstRate = gAudioEngineSpec.freq;
            int srcChans = soundBuffer->channels;
            int dstChans = gAudioEngineSpec.channels;

            // Estimate source bytes needed to produce 'length' output bytes.
            // With resampling: srcNeeded ≈ length * (srcRate / dstRate) * (srcChans / dstChans)
            // Add extra frame as safety margin.
            int srcNeeded = (int)((long long)length * srcRate * srcChans /
                                  ((long long)dstRate * dstChans)) + srcFrameSize;
            // Round up to frame boundary
            srcNeeded += srcFrameSize - 1;
            srcNeeded -= srcNeeded % srcFrameSize;

            int outputPos = 0;

            while (outputPos < length) {
                // Available source data from current position
                unsigned int available = soundBuffer->size - soundBuffer->pos;
                if (available == 0) {
                    if (soundBuffer->looping) {
                        soundBuffer->pos = 0;
                        available = soundBuffer->size;
                    } else {
                        soundBuffer->playing = false;
                        break;
                    }
                }

                // Feed a chunk of source data into the stream.
                // Don't feed more than what's available or what we estimated we need.
                int toFeed = srcNeeded;
                if (toFeed > (int)available) {
                    toFeed = available;
                }
                toFeed -= toFeed % srcFrameSize;
                if (toFeed <= 0) {
                    toFeed = srcFrameSize;
                    if (toFeed > (int)available) {
                        break;
                    }
                }

                SDL_AudioStreamPut(soundBuffer->stream,
                    (unsigned char*)soundBuffer->data + soundBuffer->pos, toFeed);
                soundBuffer->pos += toFeed;

                // Get converted data from the stream and mix it
                int remaining = length - outputPos;
                while (remaining > 0) {
                    int availConverted = SDL_AudioStreamAvailable(soundBuffer->stream);
                    if (availConverted <= 0) {
                        break;
                    }
                    unsigned char buffer[1024];
                    int toGet = remaining;
                    if (toGet > (int)sizeof(buffer)) {
                        toGet = sizeof(buffer);
                    }
                    if (toGet > availConverted) {
                        toGet = availConverted;
                    }
                    int bytesRead = SDL_AudioStreamGet(soundBuffer->stream, buffer, toGet);
                    if (bytesRead <= 0) {
                        break;
                    }
                    SDL_MixAudioFormat(stream + outputPos, buffer,
                        gAudioEngineSpec.format, bytesRead, soundBuffer->volume);
                    outputPos += bytesRead;
                    remaining = length - outputPos;
                }

                if (soundBuffer->pos >= soundBuffer->size) {
                    if (soundBuffer->looping) {
                        soundBuffer->pos %= soundBuffer->size;
                    } else {
                        // Drain remaining converted data from stream
                        while (outputPos < length) {
                            int availConverted = SDL_AudioStreamAvailable(soundBuffer->stream);
                            if (availConverted <= 0) {
                                break;
                            }
                            unsigned char buffer[1024];
                            int toGet = length - outputPos;
                            if (toGet > (int)sizeof(buffer)) {
                                toGet = sizeof(buffer);
                            }
                            int bytesRead = SDL_AudioStreamGet(soundBuffer->stream, buffer, toGet);
                            if (bytesRead <= 0) {
                                break;
                            }
                            SDL_MixAudioFormat(stream + outputPos, buffer,
                                gAudioEngineSpec.format, bytesRead, soundBuffer->volume);
                            outputPos += bytesRead;
                        }
                        soundBuffer->playing = false;
                        break;
                    }
                }

                // Recalculate how much more source data we might need
                srcNeeded = (int)((long long)(length - outputPos) * srcRate * srcChans /
                              ((long long)dstRate * dstChans)) + srcFrameSize;
                srcNeeded += srcFrameSize - 1;
                srcNeeded -= srcNeeded % srcFrameSize;
            }
        }
    }
}

bool audioEngineInit()
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == -1) {
        return false;
    }

    SDL_AudioSpec desiredSpec;
    desiredSpec.freq = 22050;
    desiredSpec.format = AUDIO_S16;
    desiredSpec.channels = 2;
    desiredSpec.samples = 1024;
    desiredSpec.callback = audioEngineMixin;

    gAudioEngineDeviceId = SDL_OpenAudioDevice(NULL, 0, &desiredSpec, &gAudioEngineSpec, SDL_AUDIO_ALLOW_ANY_CHANGE);

    if (gAudioEngineDeviceId == -1) {
        return false;
    }

    SDL_PauseAudioDevice(gAudioEngineDeviceId, 0);

    return true;
}

void audioEngineExit()
{
    if (audioEngineIsInitialized()) {
        SDL_CloseAudioDevice(gAudioEngineDeviceId);
        gAudioEngineDeviceId = -1;
    }

    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void audioEnginePause()
{
    if (audioEngineIsInitialized()) {
        SDL_PauseAudioDevice(gAudioEngineDeviceId, 1);
    }
}

void audioEngineResume()
{
    if (audioEngineIsInitialized()) {
        SDL_PauseAudioDevice(gAudioEngineDeviceId, 0);
    }
}

int audioEngineCreateSoundBuffer(unsigned int size, int bitsPerSample, int channels, int rate)
{
    if (!audioEngineIsInitialized()) {
        return -1;
    }

    for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
        AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[index]);
        std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

        if (!soundBuffer->active) {
            soundBuffer->active = true;
            soundBuffer->size = size;
            soundBuffer->bitsPerSample = bitsPerSample;
            soundBuffer->channels = channels;
            soundBuffer->rate = rate;
            soundBuffer->volume = SDL_MIX_MAXVOLUME;
            soundBuffer->playing = false;
            soundBuffer->looping = false;
            soundBuffer->pos = 0;
            soundBuffer->writePos = 0;
            soundBuffer->data = malloc(size);
            soundBuffer->stream = SDL_NewAudioStream(bitsPerSample == 16 ? AUDIO_S16 : AUDIO_S8, channels, rate, gAudioEngineSpec.format, gAudioEngineSpec.channels, gAudioEngineSpec.freq);

            return index;
        }
    }

    return -1;
}

bool audioEngineSoundBufferRelease(int soundBufferIndex)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->active = false;
    free(soundBuffer->data);
    soundBuffer->data = NULL;
    SDL_FreeAudioStream(soundBuffer->stream);
    soundBuffer->stream = NULL;

    return true;
}

bool audioEngineSoundBufferSetVolume(int soundBufferIndex, int volume)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->volume = volume;

    return true;
}

bool audioEngineSoundBufferGetVolume(int soundBufferIndex, int* volumePtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    *volumePtr = soundBuffer->volume;

    return true;
}

bool audioEngineSoundBufferSetPan(int soundBufferIndex, int pan)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    // NOTE: Audio engine does not support sound panning. I'm not sure it's
    // even needed. For now this value is silently ignored.
    return true;
}

bool audioEngineSoundBufferPlay(int soundBufferIndex, unsigned int flags)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->playing = true;

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_PLAY_LOOPING) != 0) {
        soundBuffer->looping = true;
    }

    return true;
}

bool audioEngineSoundBufferStop(int soundBufferIndex)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->playing = false;

    return true;
}

bool audioEngineSoundBufferGetCurrentPosition(int soundBufferIndex, unsigned int* readPosPtr, unsigned int* writePosPtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (readPosPtr != NULL) {
        *readPosPtr = soundBuffer->pos;
    }

    if (writePosPtr != NULL) {
        *writePosPtr = soundBuffer->writePos;
    }

    return true;
}

bool audioEngineSoundBufferSetCurrentPosition(int soundBufferIndex, unsigned int pos)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->pos = pos % soundBuffer->size;

    return true;
}

bool audioEngineSoundBufferLock(int soundBufferIndex, unsigned int writePos, unsigned int writeBytes, void** audioPtr1, unsigned int* audioBytes1, void** audioPtr2, unsigned int* audioBytes2, unsigned int flags)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (audioBytes1 == NULL) {
        return false;
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_LOCK_FROM_WRITE_POS) != 0) {
        if (!audioEngineSoundBufferGetCurrentPosition(soundBufferIndex, NULL, &writePos)) {
            return false;
        }
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_LOCK_ENTIRE_BUFFER) != 0) {
        writeBytes = soundBuffer->size;
    }

    if (writePos + writeBytes <= soundBuffer->size) {
        *(unsigned char**)audioPtr1 = (unsigned char*)soundBuffer->data + writePos;
        *audioBytes1 = writeBytes;
        if (audioPtr2 != NULL) {
            *audioPtr2 = NULL;
        }
        if (audioBytes2 != NULL) {
            *audioBytes2 = 0;
        }
    } else {
        unsigned int remainder = writePos + writeBytes - soundBuffer->size;
        *(unsigned char**)audioPtr1 = (unsigned char*)soundBuffer->data + writePos;
        *audioBytes1 = soundBuffer->size - writePos;
        if (audioPtr2 != NULL) {
            *(unsigned char**)audioPtr2 = (unsigned char*)soundBuffer->data;
        }
        if (audioBytes2 != NULL) {
            *audioBytes2 = writeBytes - (soundBuffer->size - writePos);
        }
    }

    // TODO: Mark range as locked.

    return true;
}

bool audioEngineSoundBufferUnlock(int soundBufferIndex, void* audioPtr1, unsigned int audioBytes1, void* audioPtr2, unsigned int audioBytes2)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    // Track write position for GetCurrentPosition
    soundBuffer->writePos = (unsigned int)((unsigned char*)audioPtr1 - (unsigned char*)soundBuffer->data) + audioBytes1;
    if (audioPtr2 != NULL && audioBytes2 > 0) {
        soundBuffer->writePos = audioBytes2;
    }
    if (soundBuffer->writePos >= soundBuffer->size) {
        soundBuffer->writePos %= soundBuffer->size;
    }

    // TODO: Mark range as unlocked.

    return true;
}

bool audioEngineSoundBufferGetStatus(int soundBufferIndex, unsigned int* statusPtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (statusPtr == NULL) {
        return false;
    }

    *statusPtr = 0;

    if (soundBuffer->playing) {
        *statusPtr |= AUDIO_ENGINE_SOUND_BUFFER_STATUS_PLAYING;

        if (soundBuffer->looping) {
            *statusPtr |= AUDIO_ENGINE_SOUND_BUFFER_STATUS_LOOPING;
        }
    }

    return true;
}

} // namespace fallout
