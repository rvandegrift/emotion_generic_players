#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>

#include <signal.h>

#include <vlc/vlc.h>

#include <Emotion_Generic_Plugin.h>
#include <Eina.h>
#include <Ecore.h>

static int _em_vlc_log_dom = -1;
#define ERR(...) EINA_LOG_DOM_ERR(_em_vlc_log_dom, __VA_ARGS__)
#define DBG(...) EINA_LOG_DOM_DBG(_em_vlc_log_dom, __VA_ARGS__)
#define INF(...) EINA_LOG_DOM_INFO(_em_vlc_log_dom, __VA_ARGS__)
#define WRN(...) EINA_LOG_DOM_WARN(_em_vlc_log_dom, __VA_ARGS__)
#define CRIT(...) EINA_LOG_DOM_CRIT(_em_vlc_log_dom, __VA_ARGS__)


typedef struct _App App;
struct _App {
   Emotion_Generic_Video_Shared *vs;
   Emotion_Generic_Video_Frame vf;

   libvlc_instance_t *libvlc;
   libvlc_media_t *m;
   libvlc_media_player_t *mp;
   libvlc_event_manager_t *event_mgr;

   Ecore_Pipe *fd_read;  // read commands from emotion here
   Ecore_Pipe *fd_write; // write commands for emotion here
   Eina_Lock   cmd_mutex;// lock used to send just one command at a time
   int last_order;       // current command received from emotion

   char *filename;
   char *subtitle_path;
   char *shmname;
   unsigned w, h;
   int volume;
   Eina_Bool audio_muted;

   Eina_Bool opening;
   Eina_Bool closing;
   Eina_Bool playing;
   Eina_Bool inited;
};

static void _player_setup(App *app);


/* Utilities to send commands back to emotion */
#define SEND_CMD_PARAM(app, i)                                  \
  if ((app)->fd_write)                                          \
    if (!ecore_pipe_write((app)->fd_write, &(i), sizeof((i))))  \
      ecore_main_loop_quit();

static void
_send_cmd(App *app, int cmd)
{
   if (!app->fd_write)
     return;

   eina_lock_take(&app->cmd_mutex); /* LOCK HERE */
   
   if (!ecore_pipe_write(app->fd_write, &cmd, sizeof(cmd)))
     ecore_main_loop_quit();
}

static void
_send_cmd_str(App *app, const char *str)
{
   int len;

   len = str ? strlen(str) + 1 : 0;
   if (app->fd_write)
     if (!ecore_pipe_write(app->fd_write, &len, sizeof(len)))
       ecore_main_loop_quit();
   if (app->fd_write)
     if (!ecore_pipe_write(app->fd_write, str, len))
       ecore_main_loop_quit();
}

static void
_send_cmd_finish(App *app)
{
   eina_lock_release(&app->cmd_mutex); /* UNLOCK HERE */
}

/* Commands sent to the emotion pipe */
static void
_send_file_closed(App *app)
{
   _send_cmd(app, EM_RESULT_FILE_CLOSE);
   _send_cmd_finish(app);
}

static void
_send_time_changed(App *app)
{
   float new_time;

   if (app->vs->frame_drop > 1)
     return;

   new_time = libvlc_media_player_get_time(app->mp);
   new_time /= 1000;
   _send_cmd(app, EM_RESULT_POSITION_CHANGED);
   SEND_CMD_PARAM(app, new_time);
   _send_cmd_finish(app);
}

static void
_send_resize(App *app, int width, int height)
{
   _send_cmd(app, EM_RESULT_FRAME_SIZE);
   SEND_CMD_PARAM(app, width);
   SEND_CMD_PARAM(app, height);
   _send_cmd_finish(app);
}

static void
_send_track_info(App *app, int cmd, int current, int count, libvlc_track_description_t *desc)
{
   _send_cmd(app, cmd);
   SEND_CMD_PARAM(app, current);
   SEND_CMD_PARAM(app, count);
   while (desc)
     {
        int tid = desc->i_id;
        const char *name = desc->psz_name;
        SEND_CMD_PARAM(app, tid);
        _send_cmd_str(app, name);
        desc = desc->p_next;
     }
   _send_cmd_finish(app);
}

static void
_send_all_track_info(App *app)
{
   int track_count, current;
   libvlc_track_description_t *desc;

   current = libvlc_audio_get_track(app->mp);
   track_count = libvlc_audio_get_track_count(app->mp);
   desc = libvlc_audio_get_track_description(app->mp);

   _send_track_info(app, EM_RESULT_AUDIO_TRACK_INFO,
                    current, track_count, desc);

   current = libvlc_video_get_track(app->mp);
   track_count = libvlc_video_get_track_count(app->mp);
   desc = libvlc_video_get_track_description(app->mp);

   _send_track_info(app, EM_RESULT_VIDEO_TRACK_INFO,
                    current, track_count, desc);

   current = libvlc_video_get_spu(app->mp);
   track_count = libvlc_video_get_spu_count(app->mp);
   desc = libvlc_video_get_spu_description(app->mp);

   _send_track_info(app, EM_RESULT_SPU_TRACK_INFO,
                    current, track_count, desc);
}

static void
_send_all_meta_info(App *app)
{
   const char *meta;

   _send_cmd(app, EM_RESULT_META_INFO);

   /*
    * Will send in this order: title, artist, album, year,
    * genre, comments, disc id and track count.
    */
   meta = libvlc_media_get_meta(app->m, libvlc_meta_Title);
   _send_cmd_str(app, meta);
   meta = libvlc_media_get_meta(app->m, libvlc_meta_Artist);
   _send_cmd_str(app, meta);
   meta = libvlc_media_get_meta(app->m, libvlc_meta_Album);
   _send_cmd_str(app, meta);
   meta = libvlc_media_get_meta(app->m, libvlc_meta_Date);
   _send_cmd_str(app, meta);
   meta = libvlc_media_get_meta(app->m, libvlc_meta_Genre);
   _send_cmd_str(app, meta);
   meta = NULL; // sending empty comments
   _send_cmd_str(app, meta);
   meta = NULL; // sending empty disc id
   _send_cmd_str(app, meta);
   meta = libvlc_media_get_meta(app->m, libvlc_meta_TrackNumber);
   _send_cmd_str(app, meta);

   _send_cmd_finish(app);
}

static void
_send_length_changed(App *app)
{
   float length = libvlc_media_player_get_length(app->mp);

   length /= 1000;
   _send_cmd(app, EM_RESULT_LENGTH_CHANGED);
   SEND_CMD_PARAM(app, length);
   _send_cmd_finish(app);
}

static void
_send_seekable_changed(App *app, const struct libvlc_event_t *ev)
{
   int seekable = ev->u.media_player_seekable_changed.new_seekable;

   _send_cmd(app, EM_RESULT_SEEKABLE_CHANGED);
   SEND_CMD_PARAM(app, seekable);
   _send_cmd_finish(app);
}

static void
_send_playback_started(App *app)
{
   _send_cmd(app, EM_RESULT_PLAYBACK_STARTED);
   _send_cmd_finish(app);
}

static void
_send_playback_stopped(App *app)
{
   _send_cmd(app, EM_RESULT_PLAYBACK_STOPPED);
   _send_cmd_finish(app);
}

static void
_send_init(App *app)
{
   _send_cmd(app, EM_RESULT_INIT);
   _send_cmd_finish(app);
}

static void
_send_file_set(App *app)
{
   _send_cmd(app, EM_RESULT_FILE_SET);
   _send_cmd_finish(app);
}

static void
_send_file_set_done(App *app, int success)
{
   _send_cmd(app, EM_RESULT_FILE_SET_DONE);
   SEND_CMD_PARAM(app, success);
   _send_cmd_finish(app);
}


/* VLC events and callbacks */
static void
_event_cb(const struct libvlc_event_t *ev, void *data)
{
   App *app = data;

   ecore_thread_main_loop_begin();
   switch (ev->type)
   {
      case libvlc_MediaPlayerTimeChanged:
         // DBG("libvlc_MediaPlayerTimeChanged"); 
         _send_time_changed(app);
         break;
      case libvlc_MediaPlayerLengthChanged:
         DBG("libvlc_MediaPlayerLengthChanged");
         _send_length_changed(app);
         break;
      case libvlc_MediaPlayerSeekableChanged:
         DBG("libvlc_MediaPlayerSeekableChanged");
         _send_seekable_changed(app, ev);
         break;
      case libvlc_MediaPlayerPlaying:
         DBG("libvlc_MediaPlayerPlaying");
         libvlc_audio_set_volume(app->mp, app->volume);
         libvlc_audio_set_mute(app->mp, app->audio_muted);
         _send_playback_started(app);
         break;
      case libvlc_MediaPlayerStopped:
         DBG("libvlc_MediaPlayerStopped");
         _send_playback_stopped(app);
         if (app->closing)
           {
              free(app->filename);
              app->filename = NULL;
              free(app->subtitle_path);
              app->subtitle_path = NULL;
              libvlc_media_release(app->m);
              app->m = NULL;
              libvlc_media_player_release(app->mp);
              app->mp = NULL;
              emotion_generic_shm_free(app->vs);
              app->playing = EINA_FALSE;
              app->closing = EINA_FALSE;
              _send_file_closed(app);
           }
         break;
      case libvlc_MediaPlayerEndReached:
         DBG("libvlc_MediaPlayerEndReached");
         app->playing = EINA_FALSE;
         /* vlc had released the media_playere here, we create a new one */
         app->mp = libvlc_media_player_new_from_media(app->m);
         _player_setup(app);
         _send_playback_stopped(app);
         break;
   }
   ecore_thread_main_loop_end();
}

static void
_tmp_playing_event_cb(const struct libvlc_event_t *ev, void *data)
{
   App *app = data;

   if (ev->type != libvlc_MediaPlayerPlaying)
      return;

   /* pause and stop listening the temporary event */
   libvlc_event_detach(app->event_mgr,libvlc_MediaPlayerPlaying,
                       _tmp_playing_event_cb, app);
   libvlc_media_player_set_pause(app->mp, 1);

   /* sending size info */
   libvlc_video_get_size(app->mp, 0, &app->w, &app->h);
   _send_resize(app, app->w, app->h);

   /* sending total lenght */
   _send_length_changed(app);

   /* sending audio track info */
   _send_all_track_info(app);

   /* sending meta info */
   _send_all_meta_info(app);

   /* ok, we are done! Now let emotion create the shmem for us */
   _send_file_set(app);
}

static void *
_lock(void *data, void **pixels)
{
   App *app = data;

   if (app->playing)
     *pixels = app->vf.frames[app->vs->frame.player];
   else
     *pixels = NULL;

   return NULL; // picture identifier, not needed here
}

static void
_unlock(void *data EINA_UNUSED, void *id EINA_UNUSED, void *const *pixels EINA_UNUSED)
{
}

static void
_display(void *data, void *id EINA_UNUSED)
{
   App *app = data;

   if (!app->playing)
     return;

   eina_semaphore_lock(&app->vs->lock);
   app->vs->frame.last = app->vs->frame.player;
   app->vs->frame.player = app->vs->frame.next;
   app->vs->frame.next = app->vs->frame.last;
   if (!app->vs->frame_drop++)
     {
        _send_cmd(app, EM_RESULT_FRAME_NEW);
        _send_cmd_finish(app);
     }
   eina_semaphore_release(&app->vs->lock, 1);
}

static void
_player_setup(App *app)
{
   
   libvlc_video_set_format(app->mp, "RV32", app->w, app->h, app->w * 4);
   libvlc_video_set_callbacks(app->mp, _lock, _unlock, _display, app);

   app->event_mgr = libvlc_media_player_event_manager(app->mp);
   libvlc_event_attach(app->event_mgr, libvlc_MediaPlayerPlaying,
                       _event_cb, app);
   libvlc_event_attach(app->event_mgr, libvlc_MediaPlayerTimeChanged,
                       _event_cb, app);
   libvlc_event_attach(app->event_mgr, libvlc_MediaPlayerLengthChanged,
                       _event_cb, app);
   libvlc_event_attach(app->event_mgr, libvlc_MediaPlayerSeekableChanged,
                       _event_cb, app);
   libvlc_event_attach(app->event_mgr, libvlc_MediaPlayerEndReached,
                       _event_cb, app);
   libvlc_event_attach(app->event_mgr, libvlc_MediaPlayerStopped,
                       _event_cb, app);
}

/* Commands received from the emotion pipe */
static void
_file_set(App *app)
{
   DBG("Path: %s", app->filename);
   app->m = libvlc_media_new_path(app->libvlc, app->filename);
   if (!app->m)
     {
        ERR("could not open path: \"%s\"", app->filename);
        return;
     }

   app->mp = libvlc_media_player_new_from_media(app->m);
   if (!app->mp)
     {
        ERR("could not create new player from media.");
        return;
     }

   app->opening = EINA_TRUE;

   /* Here we start playing and connect a temporary callback to know when
    * the file is parsed and ready to be played for real.
    */
   app->event_mgr = libvlc_media_player_event_manager(app->mp);
   libvlc_event_attach(app->event_mgr, libvlc_MediaPlayerPlaying,
                       _tmp_playing_event_cb, app);

   libvlc_media_player_play(app->mp);
}

static void
_file_set_done(App *app)
{
   int r;

   DBG("Path: %s", app->filename);
   app->opening = EINA_FALSE;

   r = emotion_generic_shm_get(app->shmname, &app->vs, &app->vf);
   if (!r)
     {
        free(app->filename);
        libvlc_media_release(app->m);
        libvlc_media_player_release(app->mp);
        app->filename = NULL;
        app->m = NULL;
        app->mp = NULL;
     }
   else
     {
        _player_setup(app);
     }

   _send_file_set_done(app, r);
}

static void
_file_close(App *app)
{
   DBG("closing file");

   if (!app->mp)
     return;

   app->closing = EINA_TRUE;
   libvlc_media_player_stop(app->mp);
}

static void
_stop(App *app)
{
   DBG("Stop");
   if (app->mp)
     libvlc_media_player_set_pause(app->mp, 1);
}

static void
_play(App *app, float pos)
{
   DBG("Play at %.3f", pos);

   if (!app->mp)
     return;

   if (app->playing)
     {
        libvlc_media_player_set_pause(app->mp, 0);
     }
   else
     {
        libvlc_time_t new_time = pos * 1000;
        libvlc_media_player_set_time(app->mp, new_time);
        libvlc_media_player_play(app->mp);

        if (app->subtitle_path)
          libvlc_video_set_subtitle_file(app->mp, app->subtitle_path);

        app->playing = EINA_TRUE;
     }
}

static void
_position_set(App *app, float position)
{
   libvlc_time_t new_time;

   DBG("Position set %.3f", position);
   if (!app->mp)
     return;

   new_time = position * 1000;
   libvlc_media_player_set_time(app->mp, new_time);

   if (libvlc_media_player_get_state(app->mp) == libvlc_Paused)
      _send_time_changed(app);
}

static void
_speed_set(App *app, float rate)
{
   DBG("Speed set %.3f", rate);
   if (!app->mp)
     return;

   libvlc_media_player_set_rate(app->mp, rate);
}

static void
_mute_set(App *app, int mute)
{
   DBG("Mute %d", mute);
   if (!app->mp)
     return;

   app->audio_muted = mute;
   libvlc_audio_set_mute(app->mp, mute);
}

static void
_volume_set(App *app, float volume)
{
   DBG("Volume set %.2f", volume);
   if (!app->mp)
     return;

   app->volume = volume * 100;
   libvlc_audio_set_volume(app->mp, app->volume);
}

static void
_spu_track_set(App *app, int track)
{
   DBG("SPU track %d", track);
   libvlc_video_set_spu(app->mp, track);
}

static void
_audio_track_set(App *app, int track)
{
   DBG("Audio track %d", track);
   libvlc_audio_set_track(app->mp, track);
}

static void
_video_track_set(App *app, int track)
{
   DBG("Video Track %d", track);
   libvlc_video_set_track(app->mp, track);
}

static void
_remote_command(void *data, void *buffer, unsigned int nbyte)
{
   App *app = data;

   if (nbyte == 0)
     {
        ecore_main_loop_quit();
        return ;
     }

   if (app->last_order == EM_CMD_LAST)
     {
        if (nbyte != sizeof (int))
          {
             ERR("didn't receive a valid command from emotion (%i) !", nbyte);
             ecore_main_loop_quit();
             return ;
          }

        app->last_order = *((int*) buffer);

        if (!app->inited &&
            app->last_order != EM_CMD_INIT)
          {
             ERR("wrong init command!");
             ecore_main_loop_quit();
             return ;
          }

        switch (app->last_order)
          {
           case EM_CMD_FILE_SET:
              if (app->opening)
                {
                   libvlc_media_release(app->m);
                   libvlc_media_player_release(app->mp);
                   free(app->filename);
                   app->opening = EINA_FALSE;
                }
              break;
           case EM_CMD_FILE_SET_DONE:
              _file_set_done(app);
              app->last_order = EM_CMD_LAST;
              break;
           case EM_CMD_FILE_CLOSE:
              _file_close(app);
              app->last_order = EM_CMD_LAST;
              break;
           case EM_CMD_STOP:
              _stop(app);
              app->last_order = EM_CMD_LAST;
              break;
          }
     }
   else
     {
        switch (app->last_order)
          {
           case EM_CMD_INIT:
              app->shmname = strdup(buffer);
              app->inited = EINA_TRUE;
              _send_init(app);
              break;
           case EM_CMD_FILE_SET:
              app->filename = strdup(buffer);
              _file_set(app);
              break;
           case EM_CMD_SUBTITLE_SET:
              app->subtitle_path = strdup(buffer);
              break;
           case EM_CMD_PLAY:
              _play(app, *(float*) buffer);
              break;
           case EM_CMD_POSITION_SET:
              _position_set(app, *(float*) buffer);
              break;
           case EM_CMD_SPEED_SET:
              _speed_set(app, *(float*) buffer);
              break;
           case EM_CMD_AUDIO_MUTE_SET:
              _mute_set(app, *(int*) buffer);
              break;
           case EM_CMD_VOLUME_SET:
              _volume_set(app, *(float*) buffer);
              break;
           case EM_CMD_SPU_TRACK_SET:
              _spu_track_set(app, *(int*) buffer);
              break;
           case EM_CMD_AUDIO_TRACK_SET:
              _audio_track_set(app, *(int*) buffer);
              break;
           case EM_CMD_VIDEO_TRACK_SET:
              _video_track_set(app, *(int*) buffer);
              break;
          }
        app->last_order = EM_CMD_LAST;
     }
}

static void
_dummy(void *data EINA_UNUSED, void *buffer EINA_UNUSED, unsigned int nbyte EINA_UNUSED)
{
   /* This function is useless for the pipe we use to send message back
      to emotion, but still needed */
}

/* Main */
static Eina_Bool
exit_func(void *data EINA_UNUSED, int ev_type EINA_UNUSED, void *ev EINA_UNUSED)
{
   DBG("Quit signal received !");
   ecore_main_loop_quit();
   return EINA_TRUE;
}

int
main(int argc, const char *argv[])
{
   App app;
   Ecore_Event_Handler *hld;
   int vlc_argc;

   const char *vlc_argv[] =
     {
        "--quiet",
        "--intf", "dummy",              /* no interface                      */
        "--vout", "dummy",              /* we don't want video (output)      */
        "--no-video-title-show",        /* nor the filename displayed        */
        "--no-sub-autodetect-file",     /* we don't want automatic subtitles */
        "--no-stats",                   /* no stats                          */
        "--no-inhibit",                 /* we don't want interfaces          */
        "--no-disable-screensaver",     /* we don't want interfaces          */
// XXX: causes newer vlcs to segv!
//        "--codec", "avcodec",
// XXX: disable this just in case
//        "--demux", "avformat"
     };
   vlc_argc = sizeof(vlc_argv) / sizeof(*vlc_argv);

   memset(&app, 0, sizeof(app));
   if (!eina_init())
     {
        EINA_LOG_CRIT("Can't initialize generic vlc player, eina failed.");
        return -1;
     }

   _em_vlc_log_dom = eina_log_domain_register("emotion_generic_vlc",
                                              EINA_COLOR_CYAN);
   if (_em_vlc_log_dom < 0)
     {
        EINA_LOG_CRIT("Unable to register emotion_generic_vlc log domain.");
        goto error;
     }

   if (!eina_log_domain_level_check(_em_vlc_log_dom, EINA_LOG_LEVEL_WARN))
     eina_log_domain_level_set("emotion_generic_vlc", EINA_LOG_LEVEL_WARN);

   if (argc < 3)
     {
        ERR("missing parameters.");
        ERR("syntax:\n\t%s <fd read> <fd write>", argv[0]);
        goto error;
     }

   ecore_init();

   eina_lock_new(&app.cmd_mutex);
   
   app.fd_read = ecore_pipe_full_add(_remote_command, &app,
                                     atoi(argv[1]), -1, EINA_FALSE, EINA_FALSE);
   app.fd_write = ecore_pipe_full_add(_dummy, NULL,
                                      -1, atoi(argv[2]), EINA_FALSE, EINA_FALSE);

   hld = ecore_event_handler_add(ECORE_EVENT_SIGNAL_HUP, exit_func, NULL);

   app.libvlc = libvlc_new(vlc_argc, vlc_argv);
   app.mp = NULL;
   app.filename = NULL;
   app.subtitle_path = NULL;
   app.w = 0;
   app.h = 0;
   app.opening = EINA_FALSE;
   app.playing = EINA_FALSE;
   app.inited = EINA_FALSE;
   app.last_order = EM_CMD_LAST;

   ecore_main_loop_begin();

   libvlc_release(app.libvlc);
   ecore_pipe_del(app.fd_read);
   ecore_pipe_del(app.fd_write);
   ecore_event_handler_del(hld);
   eina_lock_free(&app.cmd_mutex);

   ecore_shutdown();
   eina_shutdown();
   return 0;

 error:
   eina_shutdown();
   return -1;
}
#undef SEND_CMD_PARAM
