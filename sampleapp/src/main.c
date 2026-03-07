#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#if defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <unistd.h>
#endif
#include <vlc/vlc.h>

int main(int argc, char* argv[])
{
    (void) argc; (void) argv;
    libvlc_instance_t * inst;
    libvlc_media_player_t *mp;
    libvlc_media_t *m;

    /* Load the VLC engine */
    inst = libvlc_new (0, NULL);

    /* Create a new item */
    m = libvlc_media_new_location("http://mycool.movie.com/test.mov");

    /* Create a media player playing environement */
    mp = libvlc_media_player_new_from_media (inst, m);

    /* No need to keep the media now */
    libvlc_media_release (m);

    /* play the media_player */
    libvlc_media_player_play (mp);

    const int secondsToSleep = 10;
    while (libvlc_media_player_is_playing(mp))
    {
#if defined(WIN32)
        Sleep(secondsToSleep * 1000);
#else
        sleep(secondsToSleep);
#endif
        break;
    }

    /* Stop playing */
    libvlc_media_player_stop_async (mp);

    /* Free the media_player */
    libvlc_media_player_release (mp);

    libvlc_release (inst);

    return 0;
}
