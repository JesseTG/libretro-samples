#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "libretro.h"

#define SPEAKER_SAMPLE_RATE 44100
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define RECORDING_LENGTH 5
#define FPS 60
#define MESSAGE_DISPLAY_LENGTH (5 * FPS)
#define RECORDING_RATE_VAR "testrecording_mic_rate"

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
static uint16_t frame_buf[SCREEN_WIDTH * SCREEN_HEIGHT];

/**
 * The buffer that we'll use to store recorded audio.
 */
static int16_t *recording_buffer = NULL;

/**
 * The length of recording_buffer, in samples (not bytes).
 */
static size_t recording_buffer_length = 0;

/**
 * The buffer that we'll use for audio output.
 * Microphone input comes in mono,
 * but audio output has to be in stereo.
 */
static int16_t *playback_buffer;

/**
 * The length of playback_buffer, in samples (not bytes).
 */
static size_t playback_buffer_length = 0;

/**
 * The number of audio frames that we've recorded.
 */
static size_t frames_recorded;

/**
 * The number of audio samples (not frames or bytes) that we've played back.
 */
static size_t samples_played;

static size_t mic_samples_per_frame = 0;

/**
 * The configured sample rate for the microphone.
 */
static unsigned mic_rate = SPEAKER_SAMPLE_RATE;

static struct retro_log_callback logging = {NULL};
static retro_log_printf_t log_cb;
static struct retro_microphone_interface microphone_interface;
static retro_microphone_t *microphone = NULL;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

static void deinit(void)
{
   if (microphone_interface.close_mic)
   {
      microphone_interface.close_mic(microphone);
      microphone = NULL;
   }

   free(recording_buffer);
   free(playback_buffer);

   state = IDLE;
    frames_recorded = 0;
    samples_played = 0;
   playback_buffer_length = 0;
   recording_buffer_length = 0;
   mic_samples_per_frame = 0;
   memset(frame_buf, 0, sizeof(frame_buf));
   recording_buffer = NULL;
   playback_buffer = NULL;
}

static void init(void)
{
   struct retro_variable var = {
      .key = RECORDING_RATE_VAR,
   };

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      mic_rate = atoi(var.value);
      mic_samples_per_frame = (mic_rate / FPS);
      recording_buffer_length = mic_rate * RECORDING_LENGTH;
      recording_buffer = calloc(recording_buffer_length, sizeof(*recording_buffer));
      playback_buffer_length = mic_rate * RECORDING_LENGTH * 2; // because we output stereo
      playback_buffer = calloc(playback_buffer_length, sizeof(*playback_buffer));
      log_cb(RETRO_LOG_DEBUG, "mic_rate = %uHz\n", mic_rate);
      log_cb(RETRO_LOG_DEBUG, "mic_samples_per_frame = %u samples\n", mic_samples_per_frame);
      log_cb(RETRO_LOG_DEBUG, "recording_buffer_length = %u samples = %u bytes\n",
         recording_buffer_length,
         recording_buffer_length * sizeof(*recording_buffer));
      log_cb(RETRO_LOG_DEBUG, "playback_buffer_length = %u audio frames = %u samples = %u bytes\n",
         playback_buffer_length / 2,
         playback_buffer_length,
         playback_buffer_length * sizeof(*playback_buffer));
   }

   retro_microphone_params_t params;
   params.rate = mic_rate;
   if (microphone_interface.open_mic)
      microphone = microphone_interface.open_mic(&params);

   struct retro_message message;
   message.frames = MESSAGE_DISPLAY_LENGTH;

   if (microphone)
   {
      message.msg = "Press and hold the START button to record, release to play back.";
   }
   else
   {
      message.msg = "Failed to get microphone (is one plugged in?)";
   }

   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

void retro_init(void)
{
   init();
}

void retro_deinit(void)
{
   deinit();
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
      .sample_rate = SPEAKER_SAMPLE_RATE,
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

   struct retro_variable variables[] = {
          {
                  RECORDING_RATE_VAR,
                  "Microphone rate (reset required); 48000|44100|32000|16000|8000",
          },
          { NULL, NULL },
   };
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);

   bool no_content = true;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

   if (!log_cb)
   {
      if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
         log_cb = logging.log;
      else
         log_cb = fallback_log;
   }

   microphone_interface.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
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
   log_cb(RETRO_LOG_DEBUG, "retro_reset\n");

   deinit();
   init();
}
// RGB565
#define RED (0x1f << 11)
#define GREEN (0x3f <<  5)
#define BLUE (0x1f)
#define YELLOW (RED | GREEN)
#define WHITE (RED | GREEN | BLUE)

static void draw_lines_to_buffer(uint16_t *buf) {
   double recorded_ratio = (double)frames_recorded / (double)recording_buffer_length;
   double played_ratio = (double)samples_played / (double)playback_buffer_length;

   for (unsigned x = 0; x < SCREEN_WIDTH; x++)
   {
      double screen_fraction = (double)x / SCREEN_WIDTH;

      if (screen_fraction <= recorded_ratio)
      {
         buf[x + SCREEN_WIDTH * 110] = YELLOW;
      }

      if (screen_fraction <= played_ratio)
      {
         buf[x + SCREEN_WIDTH * 130] = BLUE;
      }
   }
}

static void render(void)
{
   memset(frame_buf, 0, sizeof(uint16_t) * SCREEN_WIDTH * SCREEN_HEIGHT); /* Black background */

   draw_lines_to_buffer(frame_buf);

   video_cb(frame_buf, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
}

static void handle_record_state(bool record_button_held) {
   int16_t* offset = recording_buffer + frames_recorded;
   ssize_t frames_left = MAX(0, recording_buffer_length - frames_recorded);
   int samples_read = microphone_interface.read_mic(microphone, offset, MIN(frames_left, mic_samples_per_frame));
   if (samples_read < 0)
   { // If there was a problem querying the mic...
      log_cb(RETRO_LOG_DEBUG, "Entering ERROR state (error reading microphone)\n");
      microphone_interface.set_mic_state(microphone, false);
      state = ERROR;
   }
   else
   {
      frames_recorded += samples_read;

      if (!record_button_held || (frames_recorded >= recording_buffer_length))
      { // If the mic button was released, or if we've filled the recording buffer...

         memset(playback_buffer, 0, playback_buffer_length * sizeof(*playback_buffer));
         for (int i = 0; i < MIN(frames_recorded, recording_buffer_length); ++i)
         {
            playback_buffer[i * 2] = recording_buffer[i];
            playback_buffer[i * 2 + 1] = recording_buffer[i];
         }
         samples_played = 0;
         microphone_interface.set_mic_state(microphone, false);
         // Shut off the mic, we won't use it during playback
         log_cb(RETRO_LOG_DEBUG, "Entering PLAYBACK state (mic buffer is full or button was released)\n");
         state = PLAYBACK;
      }
   }
}

void handle_playback_state(void) {
   const int16_t* offset = playback_buffer + samples_played;
   size_t samples_left = MIN(frames_recorded * 2, playback_buffer_length) - samples_played;
   size_t samples_to_play = MIN(samples_left, mic_samples_per_frame);
   // Submitting too much audio will cause the main thread to block while it plays
   size_t frames_written = audio_batch_cb(offset, samples_to_play);
   samples_played += frames_written;

   if (samples_played >= playback_buffer_length || samples_played >= (frames_recorded * 2))
   {
      log_cb(RETRO_LOG_DEBUG, "Entering FINISHED_PLAYBACK state (finished playing audio data)\n");
      state = FINISHED_PLAYBACK;
   }
}

void retro_run(void)
{
   input_poll_cb();

   bool record_button = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);

   switch (state) {
      case IDLE:
         if (microphone && microphone_interface.set_mic_state && record_button)
         { // If we're not doing anything, and the microphone is valid, and the record button is pressed down...
            frames_recorded = 0;
             samples_played = 0;
            memset(recording_buffer, 0, recording_buffer_length * sizeof(*recording_buffer));
            memset(playback_buffer, 0, playback_buffer_length * sizeof(*playback_buffer));
            if (microphone_interface.set_mic_state(microphone, true))
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
          frames_recorded = 0;
           samples_played = 0;
         break;
      case RECORDING:
         if (audio_batch_cb)
         {
            handle_record_state(record_button);
         }
         break;
      case PLAYBACK:
         if (audio_batch_cb)
         {
            handle_playback_state();
         }
         break;
      case FINISHED_PLAYBACK:
          samples_played = 0;
           frames_recorded = 0;
         state = IDLE;
         log_cb(RETRO_LOG_DEBUG, "Entering IDLE state (ready for more audio input)\n");
         break;
   }

   render();
}

bool retro_load_game(const struct retro_game_info *info)
{
   log_cb(RETRO_LOG_DEBUG, "retro_load_game\n");

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_INFO, "RGB565 is not supported.\n");
      return false;
   }

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

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data_, size_t size)
{
   return false;
}

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

