#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include "libretro.h"

#define ARRAY_LENGTH(x) (sizeof(x) / sizeof((x)[0]))
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define SAMPLE_RATE 44100
#define RECORDING_LENGTH 5
#define FPS 60
#define SAMPLES_PER_FRAME (SAMPLE_RATE / FPS)
#define MESSAGE_DISPLAY_LENGTH (5 * FPS)

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) > (b) ? (b) : (a))

enum state {
   IDLE,
   ERROR,
   RECORDING,
   PLAYBACK,
   FINISHED_PLAYBACK,
};

static enum state state = IDLE;

/**
 * The pixels that we'll draw the visualization to.
 */
static uint32_t frame_buf[SCREEN_WIDTH * SCREEN_HEIGHT];

/**
 * The buffer that we'll use to store recorded audio.
 */
static int16_t recording_buffer[SAMPLE_RATE * RECORDING_LENGTH];

/**
 * The buffer that we'll use for audio output.
 * Microphone input comes in mono,
 * but audio output has to be in stereo.
 */
static int16_t playback_buffer[SAMPLE_RATE * RECORDING_LENGTH * 2];

/**
 * The number of audio frames that we've recorded.
 */
static size_t samples_recorded;

/**
 * The number of audio frames that we've played back.
 */
static size_t samples_played;

static struct retro_log_callback logging;
static retro_log_printf_t log_cb;
static struct retro_microphone_interface microphone_interface;
static retro_microphone_t *microphone = NULL;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static int16_t SILENCE[] = {0, 0};

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

void retro_init(void)
{
   state = IDLE;
   samples_recorded = 0;
   samples_played = 0;
   memset(frame_buf, 0, sizeof(frame_buf));
   memset(recording_buffer, 0, sizeof(recording_buffer));
   memset(playback_buffer, 0, sizeof(playback_buffer));
}

void retro_deinit(void)
{
   if (microphone_interface.free_microphone)
   {
      microphone_interface.free_microphone(microphone);
      microphone = NULL;
   }
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Audio Recording Sample Core";
   info->library_version  = "1";
   info->need_fullpath    = false;
   info->valid_extensions = NULL; // Anything is fine, we don't care.
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   float aspect = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;

   info->timing = (struct retro_system_timing) {
      .fps = FPS,
      .sample_rate = SAMPLE_RATE,
   };

   info->geometry = (struct retro_game_geometry) {
      .base_width   = SCREEN_WIDTH,
      .base_height  = SCREEN_HEIGHT,
      .max_width    = SCREEN_WIDTH,
      .max_height   = SCREEN_HEIGHT,
      .aspect_ratio = aspect,
   };
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    bool no_content = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    else
        log_cb = fallback_log;

    if (!cb(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &microphone_interface))
    {
       struct retro_message message;
       message.msg = "Failed to get microphone interface";
       message.frames = MESSAGE_DISPLAY_LENGTH;
       cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
    }
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
   samples_recorded = 0;
   samples_played = 0;
   state = IDLE;
   memset(frame_buf, 0, sizeof(frame_buf));
   memset(recording_buffer, 0, sizeof(recording_buffer));
   memset(playback_buffer, 0, sizeof(playback_buffer));

   if (microphone && microphone_interface.free_microphone)
      microphone_interface.free_microphone(microphone);

   microphone = NULL;
}

#define RED (0xff << 16)
#define GREEN (0xff <<  8)
#define BLUE (0xff)
#define YELLOW (RED | GREEN)
#define WHITE (RED | GREEN | BLUE)

static void render(void)
{
   uint32_t *buf = frame_buf;
   memset(buf, 0, sizeof(uint32_t) * 320 * 240); /* Black background */

   for (unsigned x = 0; x < SCREEN_WIDTH; x++)
   {
      buf[x + SCREEN_WIDTH * 32] = WHITE;
   }

   {
      double recorded_ratio = (double)samples_recorded / (double)ARRAY_LENGTH(recording_buffer);
      for (unsigned x = 0; x < SCREEN_WIDTH; x++)
      {
         double screen_fraction = (double)x / SCREEN_WIDTH;

         if (screen_fraction <= recorded_ratio)
         {
            buf[x + SCREEN_WIDTH * 110] = YELLOW;
         }
      }
   }

   {
      double played_ratio = (double)samples_played / (double)ARRAY_LENGTH(playback_buffer);
      for (unsigned x = 0; x < SCREEN_WIDTH; x++)
      {
         double screen_fraction = (double)x / SCREEN_WIDTH;

         if (screen_fraction <= played_ratio)
         {
            buf[x + SCREEN_WIDTH * 130] = BLUE;
         }
      }
   }

   video_cb(buf, 320, 240, SCREEN_WIDTH << 2);
}

void retro_run(void)
{
   input_poll_cb();

   bool record_button = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);

   switch (state) {
      case IDLE:
         if (audio_batch_cb)
         {
            audio_batch_cb(SILENCE, 1);
         }
         if (microphone && microphone_interface.set_microphone_state && record_button)
         { // If we're not doing anything, and the microphone is valid, and the record button is pressed down...
            samples_recorded = 0;
            samples_played = 0;
            memset(recording_buffer, 0, sizeof(recording_buffer));
            memset(playback_buffer, 0, sizeof(playback_buffer));
            if (microphone_interface.set_microphone_state(microphone, true))
            { // If we successfully started the microphone...
               log_cb(RETRO_LOG_DEBUG, "Entering RECORDING state\n");
               state = RECORDING;
            }
            else
            {
               log_cb(RETRO_LOG_DEBUG, "Entering ERROR state (failed to enable mic)\n");
               state = ERROR;
            }
         }
         break;
      case ERROR:
         samples_recorded = 0;
         samples_played = 0;
         if (audio_batch_cb)
         {
            audio_batch_cb(SILENCE, 1);
         }
         break;
      case RECORDING:
         if (audio_batch_cb)
         {
            int16_t* offset = recording_buffer + samples_recorded;
            ssize_t frames_left = MAX(0, ARRAY_LENGTH(recording_buffer) - samples_recorded);
            int samples_read = microphone_interface.get_microphone_input(microphone, offset, MIN(frames_left, SAMPLES_PER_FRAME));
            if (samples_read < 0)
            { // If there was a problem querying the mic...
               log_cb(RETRO_LOG_DEBUG, "Entering ERROR state (error reading microphone)\n");
               microphone_interface.set_microphone_state(microphone, false);
               state = ERROR;
            }
            else
            {
               samples_recorded += samples_read;

               if (!record_button || (samples_recorded >= ARRAY_LENGTH(recording_buffer)))
               { // If the mic button was released, or if we've filled the recording buffer...

                  memset(playback_buffer, 0, sizeof(playback_buffer));
                  for (int i = 0; i < MIN(samples_recorded, ARRAY_LENGTH(recording_buffer)); ++i)
                  {
                     playback_buffer[i * 2] = recording_buffer[i];
                     playback_buffer[i * 2 + 1] = recording_buffer[i];
                  }
                  samples_played = 0;
                  microphone_interface.set_microphone_state(microphone, false);
                  // Shut off the mic, we won't use it during playback
                  log_cb(RETRO_LOG_DEBUG, "Entering PLAYBACK state (mic buffer is full or button was released)\n");
                  state = PLAYBACK;
               }
            }
            audio_batch_cb(SILENCE, 1);
            /* Need to call this even if no audio is playing, as it flushes all buffers */
         }
         break;
      case PLAYBACK:
         if (audio_batch_cb)
         {
            const int16_t* offset = playback_buffer + samples_played;
            size_t frames_left = MIN(samples_recorded * 2, ARRAY_LENGTH(playback_buffer)) - samples_played;
            frames_left = MIN(frames_left, SAMPLES_PER_FRAME * 2); // times 2 because we're converting mono input to stereo output
            // Submitting too much audio will cause the main thread to block while it plays
            size_t frames_written = audio_batch_cb(offset, frames_left);
            samples_played += frames_written;

            if (samples_played >= ARRAY_LENGTH(playback_buffer) || samples_played >= (samples_recorded * 2))
            {
               log_cb(RETRO_LOG_DEBUG, "Entering FINISHED_PLAYBACK state (finished playing audio data)\n");
               state = FINISHED_PLAYBACK;
            }
         }
         break;
      case FINISHED_PLAYBACK:
         samples_played = 0;
         samples_recorded = 0;
         state = IDLE;
         audio_batch_cb(SILENCE, 1);
         log_cb(RETRO_LOG_DEBUG, "Entering IDLE state (ready for more audio input)\n");
         break;
   }

   render();
}

bool retro_load_game(const struct retro_game_info *info)
{
   struct retro_message message;
   message.frames = MESSAGE_DISPLAY_LENGTH;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported.\n");
      return false;
   }

   if (microphone_interface.init_microphone)
      microphone = microphone_interface.init_microphone();

   if (microphone)
   {
      message.msg = "Press and hold the START button to record, release to play back.";
   }
   else
   {
      message.msg = "Failed to get microphone (is one plugged in?)";
   }

   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);

   return true;
}

void retro_unload_game(void)
{
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   if (type != 0x200)
      return false;
   if (num != 2)
      return false;
   return retro_load_game(NULL);
}

// TODO: Implement
size_t retro_serialize_size(void)
{
   return
         sizeof(frame_buf) +
         sizeof(recording_buffer) +
         sizeof(playback_buffer) +
         sizeof(state) +
         sizeof(samples_recorded) +
         sizeof(samples_played);
}

// TODO: Implement
bool retro_serialize(void *data_, size_t size)
{
   return false;
}

// TODO: Implement
bool retro_unserialize(const void *data_, size_t size)
{
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

