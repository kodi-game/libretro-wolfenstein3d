/*
  SDL_mixer:  An audio mixer library based on the SDL library
  Copyright (C) 1997-2013 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* $Id$ */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "SDL_endian.h"
#include "SDL_audio.h"
#include "SDL_timer.h"

#include "SDL_mixer.h"

#ifdef WAV_MUSIC
#include "wavestream.h"
#endif

int volatile music_active = 1;
static int volatile music_stopped = 0;
static int music_loops = 0;
static char *music_cmd = NULL;
static Mix_Music * volatile music_playing = NULL;
static int music_volume = MIX_MAX_VOLUME;

struct _Mix_Music {
    Mix_MusicType type;
    union {
#ifdef WAV_MUSIC
        WAVStream *wave;
#endif
    } data;
    Mix_Fading fading;
    int fade_step;
    int fade_steps;
    int error;
};

/* Used to calculate fading steps */
static int ms_per_step;

/* rcg06042009 report available decoders at runtime. */
static const char **music_decoders = NULL;
static int num_decoders = 0;

/* Semicolon-separated SoundFont paths */

int Mix_GetNumMusicDecoders(void)
{
    return(num_decoders);
}

const char *Mix_GetMusicDecoder(int index)
{
    if ((index < 0) || (index >= num_decoders)) {
        return NULL;
    }
    return(music_decoders[index]);
}

static void add_music_decoder(const char *decoder)
{
    void *ptr = SDL_realloc((void *)music_decoders, (num_decoders + 1) * sizeof (const char *));
    if (ptr == NULL) {
        return;  /* oh well, go on without it. */
    }
    music_decoders = (const char **) ptr;
    music_decoders[num_decoders++] = decoder;
}

/* Local low-level functions prototypes */
static void music_internal_initialize_volume(void);
static void music_internal_volume(int volume);
static int  music_internal_play(Mix_Music *music, double position);
static int  music_internal_position(double position);
static int  music_internal_playing();
static void music_internal_halt(void);


/* Support for hooking when the music has finished */
static void (*music_finished_hook)(void) = NULL;

void Mix_HookMusicFinished(void (*music_finished)(void))
{
    SDL_LockAudio();
    music_finished_hook = music_finished;
    SDL_UnlockAudio();
}


/* If music isn't playing, halt it if no looping is required, restart it */
/* othesrchise. NOP if the music is playing */
static int music_halt_or_loop (void)
{
    /* Restart music if it has to loop */

    if (!music_internal_playing())
    {
        /* Restart music if it has to loop at a high level */
        if (music_loops)
        {
            Mix_Fading current_fade;
            if (music_loops > 0) {
                --music_loops;
            }
            current_fade = music_playing->fading;
            music_internal_play(music_playing, 0.0);
            music_playing->fading = current_fade;
        }
        else
        {
            music_internal_halt();
            if (music_finished_hook)
                music_finished_hook();

            return 0;
        }
    }

    return 1;
}



/* Mixing function */
void music_mixer(void *udata, Uint8 *stream, int len)
{
    int left = 0;

    if ( music_playing && music_active ) {
        /* Handle fading */
        if ( music_playing->fading != MIX_NO_FADING ) {
            if ( music_playing->fade_step++ < music_playing->fade_steps ) {
                int volume;
                int fade_step = music_playing->fade_step;
                int fade_steps = music_playing->fade_steps;

                if ( music_playing->fading == MIX_FADING_OUT ) {
                    volume = (music_volume * (fade_steps-fade_step)) / fade_steps;
                } else { /* Fading in */
                    volume = (music_volume * fade_step) / fade_steps;
                }
                music_internal_volume(volume);
            } else {
                if ( music_playing->fading == MIX_FADING_OUT ) {
                    music_internal_halt();
                    if ( music_finished_hook ) {
                        music_finished_hook();
                    }
                    return;
                }
                music_playing->fading = MIX_NO_FADING;
            }
        }

        music_halt_or_loop();
        if (!music_internal_playing())
            return;

        switch (music_playing->type) {
#ifdef WAV_MUSIC
            case MUS_WAV:
                left = WAVStream_PlaySome(stream, len);
                break;
#endif
            default:
                /* Unknown music type?? */
                break;
        }
    }

skip:
    /* Handle seamless music looping */
    if (left > 0 && left < len) {
        music_halt_or_loop();
        if (music_internal_playing())
            music_mixer(udata, stream+(len-left), left);
    }
}

/* Initialize the music players with a certain desired audio format */
int open_music(SDL_AudioSpec *mixer)
{
#ifdef WAV_MUSIC
    if ( WAVStream_Init(mixer) == 0 ) {
        add_music_decoder("WAVE");
    }
#endif
    music_playing = NULL;
    music_stopped = 0;
    Mix_VolumeMusic(SDL_MIX_MAXVOLUME);

    /* Calculate the number of ms for each callback */
    ms_per_step = (int) (((float)mixer->samples * 1000.0) / mixer->freq);

    return(0);
}

/* Portable case-insensitive string compare function */
int MIX_string_equals(const char *str1, const char *str2)
{
    while ( *str1 && *str2 ) {
        if ( toupper((unsigned char)*str1) !=
             toupper((unsigned char)*str2) )
            break;
        ++str1;
        ++str2;
    }
    return (!*str1 && !*str2);
}

static int detect_mp3(Uint8 *magic)
{
    if ( strncmp((char *)magic, "ID3", 3) == 0 ) {
        return 1;
    }

    /* Detection code lifted from SMPEG */
    if(((magic[0] & 0xff) != 0xff) || // No sync bits
       ((magic[1] & 0xf0) != 0xf0) || //
       ((magic[2] & 0xf0) == 0x00) || // Bitrate is 0
       ((magic[2] & 0xf0) == 0xf0) || // Bitrate is 15
       ((magic[2] & 0x0c) == 0x0c) || // Frequency is 3
       ((magic[1] & 0x06) == 0x00)) { // Layer is 4
        return(0);
    }
    return 1;
}

/* MUS_MOD can't be auto-detected. If no other format was detected, MOD is
 * assumed and MUS_MOD will be returned, meaning that the format might not
 * actually be MOD-based.
 *
 * Returns MUS_NONE in case of errors. */
static Mix_MusicType detect_music_type(SDL_RWops *src)
{
    Uint8 magic[5];
    Uint8 moremagic[9];

    Sint64 start = SDL_RWtell(src);
    if (SDL_RWread(src, magic, 1, 4) != 4 || SDL_RWread(src, moremagic, 1, 8) != 8 ) {
        Mix_SetError("Couldn't read from RWops");
        return MUS_NONE;
    }
    SDL_RWseek(src, start, RW_SEEK_SET);
    magic[4]='\0';
    moremagic[8] = '\0';

    /* WAVE files have the magic four bytes "RIFF"
       AIFF files have the magic 12 bytes "FORM" XXXX "AIFF" */
    if (((strcmp((char *)magic, "RIFF") == 0) && (strcmp((char *)(moremagic+4), "WAVE") == 0)) ||
        (strcmp((char *)magic, "FORM") == 0)) {
        return MUS_WAV;
    }

    /* Ogg Vorbis files have the magic four bytes "OggS" */
    if (strcmp((char *)magic, "OggS") == 0) {
        return MUS_OGG;
    }

    /* FLAC files have the magic four bytes "fLaC" */
    if (strcmp((char *)magic, "fLaC") == 0) {
        return MUS_FLAC;
    }

    /* MIDI files have the magic four bytes "MThd" */
    if (strcmp((char *)magic, "MThd") == 0) {
        return MUS_MID;
    }

    if (detect_mp3(magic)) {
        return MUS_MP3;
    }

    /* Assume MOD format.
     *
     * Apparently there is no way to check if the file is really a MOD,
     * or there are too many formats supported by MikMod/ModPlug, or
     * MikMod/ModPlug does this check by itself. */
    return MUS_MOD;
}

/* Load a music file */
Mix_Music *Mix_LoadMUS(const char *file)
{
    SDL_RWops *src;
    Mix_Music *music;
    Mix_MusicType type;
    char *ext = strrchr(file, '.');


    src = SDL_RWFromFile(file, "rb");
    if ( src == NULL ) {
        Mix_SetError("Couldn't open '%s'", file);
        return NULL;
    }

    /* Use the extension as a first guess on the file type */
    type = MUS_NONE;
    ext = strrchr(file, '.');
    /* No need to guard these with #ifdef *_MUSIC stuff,
     * since we simply call Mix_LoadMUSType_RW() later */
    if ( ext ) {
        ++ext; /* skip the dot in the extension */
        if ( MIX_string_equals(ext, "WAV") ) {
            type = MUS_WAV;
        } else if ( MIX_string_equals(ext, "MID") ||
                    MIX_string_equals(ext, "MIDI") ||
                    MIX_string_equals(ext, "KAR") ) {
            type = MUS_MID;
        } else if ( MIX_string_equals(ext, "OGG") ) {
            type = MUS_OGG;
        } else if ( MIX_string_equals(ext, "FLAC") ) {
            type = MUS_FLAC;
        } else  if ( MIX_string_equals(ext, "MPG") ||
                     MIX_string_equals(ext, "MPEG") ||
                     MIX_string_equals(ext, "MP3") ||
                     MIX_string_equals(ext, "MAD") ) {
            type = MUS_MP3;
        }
    }
    if ( type == MUS_NONE ) {
        type = detect_music_type(src);
    }

    /* We need to know if a specific error occurs; if not, we'll set a
     * generic one, so we clear the current one. */
    Mix_SetError("");
    music = Mix_LoadMUSType_RW(src, type, SDL_TRUE);
    if ( music == NULL && Mix_GetError()[0] == '\0' ) {
        Mix_SetError("Unrecognized music format");
    }
    return music;
}

Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc)
{
    return Mix_LoadMUSType_RW(src, MUS_NONE, freesrc);
}

Mix_Music *Mix_LoadMUSType_RW(SDL_RWops *src, Mix_MusicType type, int freesrc)
{
    Mix_Music *music;
    Sint64 start;

    if (!src) {
        Mix_SetError("RWops pointer is NULL");
        return NULL;
    }
    start = SDL_RWtell(src);

    /* If the caller wants auto-detection, figure out what kind of file
     * this is. */
    if (type == MUS_NONE) {
        if ((type = detect_music_type(src)) == MUS_NONE) {
            /* Don't call Mix_SetError() here since detect_music_type()
             * does that. */
            if (freesrc) {
                SDL_RWclose(src);
            }
            return NULL;
        }
    }

    /* Allocate memory for the music structure */
    music = (Mix_Music *)SDL_malloc(sizeof(Mix_Music));
    if (music == NULL ) {
        Mix_SetError("Out of memory");
        if (freesrc) {
            SDL_RWclose(src);
        }
        return NULL;
    }
    music->error = 1;

    switch (type) {
#ifdef WAV_MUSIC
    case MUS_WAV:
        music->type = MUS_WAV;
        music->data.wave = WAVStream_LoadSong_RW(src, freesrc);
        if (music->data.wave) {
            music->error = 0;
        }
        break;
#endif

    default:
        Mix_SetError("Unrecognized music format");
        break;
    } /* switch (want) */

    if (music->error) {
        SDL_free(music);
        if (freesrc) {
            SDL_RWclose(src);
        } else {
            SDL_RWseek(src, start, RW_SEEK_SET);
        }
        music = NULL;
    }
    return music;
}

/* Free a music chunk previously loaded */
void Mix_FreeMusic(Mix_Music *music)
{
    if ( music ) {
        /* Stop the music if it's currently playing */
        SDL_LockAudio();
        if ( music == music_playing ) {
            /* Wait for any fade out to finish */
            while ( music->fading == MIX_FADING_OUT ) {
                SDL_UnlockAudio();
                SDL_Delay(100);
                SDL_LockAudio();
            }
            if ( music == music_playing ) {
                music_internal_halt();
            }
        }
        SDL_UnlockAudio();
        switch (music->type) {
#ifdef WAV_MUSIC
            case MUS_WAV:
                WAVStream_FreeSong(music->data.wave);
                break;
#endif
            default:
                /* Unknown music type?? */
                break;
        }

    skip:
        SDL_free(music);
    }
}

/* Find out the music format of a mixer music, or the currently playing
   music, if 'music' is NULL.
*/
Mix_MusicType Mix_GetMusicType(const Mix_Music *music)
{
    Mix_MusicType type = MUS_NONE;

    if ( music ) {
        type = music->type;
    } else {
        SDL_LockAudio();
        if ( music_playing ) {
            type = music_playing->type;
        }
        SDL_UnlockAudio();
    }
    return(type);
}

/* Play a music chunk.  Returns 0, or -1 if there was an error.
 */
static int music_internal_play(Mix_Music *music, double position)
{
    int retval = 0;

    /* Note the music we're playing */
    if ( music_playing ) {
        music_internal_halt();
    }
    music_playing = music;

    /* Set the initial volume */
    if ( music->type != MUS_MOD ) {
        music_internal_initialize_volume();
    }

    /* Set up for playback */
    switch (music->type) {
#ifdef WAV_MUSIC
        case MUS_WAV:
        WAVStream_Start(music->data.wave);
        break;
#endif
        default:
        Mix_SetError("Can't play unknown music type");
        retval = -1;
        break;
    }

skip:
    /* Set the playback position, note any errors if an offset is used */
    if ( retval == 0 ) {
        if ( position > 0.0 ) {
            if ( music_internal_position(position) < 0 ) {
                Mix_SetError("Position not implemented for music type");
                retval = -1;
            }
        } else {
            music_internal_position(0.0);
        }
    }

    /* If the setup failed, we're not playing any music anymore */
    if ( retval < 0 ) {
        music_playing = NULL;
    }
    return(retval);
}
int Mix_FadeInMusicPos(Mix_Music *music, int loops, int ms, double position)
{
    int retval;

    if ( ms_per_step == 0 ) {
        SDL_SetError("Audio device hasn't been opened");
        return(-1);
    }

    /* Don't play null pointers :-) */
    if ( music == NULL ) {
        Mix_SetError("music parameter was NULL");
        return(-1);
    }

    /* Setup the data */
    if ( ms ) {
        music->fading = MIX_FADING_IN;
    } else {
        music->fading = MIX_NO_FADING;
    }
    music->fade_step = 0;
    music->fade_steps = ms/ms_per_step;

    /* Play the puppy */
    SDL_LockAudio();
    /* If the current music is fading out, wait for the fade to complete */
    while ( music_playing && (music_playing->fading == MIX_FADING_OUT) ) {
        SDL_UnlockAudio();
        SDL_Delay(100);
        SDL_LockAudio();
    }
    music_active = 1;
    if (loops == 1) {
        /* Loop is the number of times to play the audio */
        loops = 0;
    }
    music_loops = loops;
    retval = music_internal_play(music, position);
    SDL_UnlockAudio();

    return(retval);
}
int Mix_FadeInMusic(Mix_Music *music, int loops, int ms)
{
    return Mix_FadeInMusicPos(music, loops, ms, 0.0);
}
int Mix_PlayMusic(Mix_Music *music, int loops)
{
    return Mix_FadeInMusicPos(music, loops, 0, 0.0);
}

/* Set the playing music position */
int music_internal_position(double position)
{
    int retval = 0;

    switch (music_playing->type) {
        default:
        /* TODO: Implement this for other music backends */
        retval = -1;
        break;
    }
    return(retval);
}
int Mix_SetMusicPosition(double position)
{
    int retval;

    SDL_LockAudio();
    if ( music_playing ) {
        retval = music_internal_position(position);
        if ( retval < 0 ) {
            Mix_SetError("Position not implemented for music type");
        }
    } else {
        Mix_SetError("Music isn't playing");
        retval = -1;
    }
    SDL_UnlockAudio();

    return(retval);
}

/* Set the music's initial volume */
static void music_internal_initialize_volume(void)
{
    if ( music_playing->fading == MIX_FADING_IN ) {
        music_internal_volume(0);
    } else {
        music_internal_volume(music_volume);
    }
}

/* Set the music volume */
static void music_internal_volume(int volume)
{
    switch (music_playing->type) {
#ifdef WAV_MUSIC
        case MUS_WAV:
        WAVStream_SetVolume(volume);
        break;
#endif
        default:
        /* Unknown music type?? */
        break;
    }
}
int Mix_VolumeMusic(int volume)
{
    int prev_volume;

    prev_volume = music_volume;
    if ( volume < 0 ) {
        return prev_volume;
    }
    if ( volume > SDL_MIX_MAXVOLUME ) {
        volume = SDL_MIX_MAXVOLUME;
    }
    music_volume = volume;
    SDL_LockAudio();
    if ( music_playing ) {
        music_internal_volume(music_volume);
    }
    SDL_UnlockAudio();
    return(prev_volume);
}

/* Halt playing of music */
static void music_internal_halt(void)
{
    switch (music_playing->type) {
#ifdef WAV_MUSIC
        case MUS_WAV:
        WAVStream_Stop();
        break;
#endif
        default:
        /* Unknown music type?? */
        return;
    }

skip:
    music_playing->fading = MIX_NO_FADING;
    music_playing = NULL;
}
int Mix_HaltMusic(void)
{
    SDL_LockAudio();
    if ( music_playing ) {
        music_internal_halt();
        if ( music_finished_hook ) {
            music_finished_hook();
        }
    }
    SDL_UnlockAudio();

    return(0);
}

/* Progressively stop the music */
int Mix_FadeOutMusic(int ms)
{
    int retval = 0;

    if ( ms_per_step == 0 ) {
        SDL_SetError("Audio device hasn't been opened");
        return 0;
    }

    if (ms <= 0) {  /* just halt immediately. */
        Mix_HaltMusic();
        return 1;
    }

    SDL_LockAudio();
    if ( music_playing) {
                int fade_steps = (ms + ms_per_step - 1)/ms_per_step;
                if ( music_playing->fading == MIX_NO_FADING ) {
                music_playing->fade_step = 0;
                } else {
                        int step;
                        int old_fade_steps = music_playing->fade_steps;
                        if ( music_playing->fading == MIX_FADING_OUT ) {
                                step = music_playing->fade_step;
                        } else {
                                step = old_fade_steps
                                        - music_playing->fade_step + 1;
                        }
                        music_playing->fade_step = (step * fade_steps)
                                / old_fade_steps;
                }
        music_playing->fading = MIX_FADING_OUT;
        music_playing->fade_steps = fade_steps;
        retval = 1;
    }
    SDL_UnlockAudio();

    return(retval);
}

Mix_Fading Mix_FadingMusic(void)
{
    Mix_Fading fading = MIX_NO_FADING;

    SDL_LockAudio();
    if ( music_playing ) {
        fading = music_playing->fading;
    }
    SDL_UnlockAudio();

    return(fading);
}

/* Pause/Resume the music stream */
void Mix_PauseMusic(void)
{
    music_active = 0;
}

void Mix_ResumeMusic(void)
{
    music_active = 1;
}

void Mix_RewindMusic(void)
{
    Mix_SetMusicPosition(0.0);
}

int Mix_PausedMusic(void)
{
    return (music_active == 0);
}

/* Check the status of the music */
static int music_internal_playing()
{
    int playing = 1;

    if (music_playing == NULL) {
        return 0;
    }

    switch (music_playing->type) {
#ifdef WAV_MUSIC
        case MUS_WAV:
        if ( ! WAVStream_Active() ) {
            playing = 0;
        }
        break;
#endif
        default:
        playing = 0;
        break;
    }

skip:
    return(playing);
}
int Mix_PlayingMusic(void)
{
    int playing = 0;

    SDL_LockAudio();
    if ( music_playing ) {
        playing = music_loops || music_internal_playing();
    }
    SDL_UnlockAudio();

    return(playing);
}

/* Set the external music playback command */
int Mix_SetMusicCMD(const char *command)
{
    Mix_HaltMusic();
    if ( music_cmd ) {
        SDL_free(music_cmd);
        music_cmd = NULL;
    }
    if ( command ) {
        music_cmd = (char *)SDL_malloc(strlen(command)+1);
        if ( music_cmd == NULL ) {
            return(-1);
        }
        strcpy(music_cmd, command);
    }
    return(0);
}

int Mix_SetSynchroValue(int i)
{
    /* Not supported by any players at this time */
    return(-1);
}

int Mix_GetSynchroValue(void)
{
    /* Not supported by any players at this time */
    return(-1);
}


/* Uninitialize the music players */
void close_music(void)
{
    Mix_HaltMusic();

    /* rcg06042009 report available decoders at runtime. */
    SDL_free((void *)music_decoders);
    music_decoders = NULL;
    num_decoders = 0;

    ms_per_step = 0;
}

int Mix_SetSoundFonts(const char *paths)
{
    return 1;
}
