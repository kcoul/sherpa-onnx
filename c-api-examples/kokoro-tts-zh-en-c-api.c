#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sherpa-onnx/c-api/c-api.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#ifndef EVAL_KOKORO_MODEL_DIR
#define EVAL_KOKORO_MODEL_DIR "./kokoro-multi-lang-v1_1"
#endif

#ifndef EVAL_KOKORO_TEST_INPUT
#define EVAL_KOKORO_TEST_INPUT "./kokoro-multi-lang-v1_1/test_input.txt"
#endif

#ifndef EVAL_KOKORO_OUTPUT_WAV
#define EVAL_KOKORO_OUTPUT_WAV "./generated-kokoro-zh-en.wav"
#endif

static void PrintUsage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [--model-dir PATH] [--text-file PATH] [--output PATH] "
          "[--sid N] [--speed F] [--debug 0|1] [--no-playback] "
          "[--include-zh-lexicon]\n",
          prog);
}

static int ParseInt(const char *s, int32_t *out) {
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0') return 0;
  *out = (int32_t)v;
  return 1;
}

static int ParseFloat(const char *s, float *out) {
  char *end = NULL;
  double v = strtod(s, &end);
  if (end == s || *end != '\0') return 0;
  *out = (float)v;
  return 1;
}

static char *ReadEntireFile(const char *path) {
  FILE *f = fopen(path, "rb");
  long size;
  char *buf;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  buf = (char *)malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[size] = '\0';
  fclose(f);
  return buf;
}

static int ReplaceAll(char *s, const char *from, const char *to) {
  size_t from_len = strlen(from);
  size_t to_len = strlen(to);
  char *p;
  int changed = 0;
  if (from_len == 0 || from_len != to_len) return 0;
  p = strstr(s, from);
  while (p) {
    memcpy(p, to, to_len);
    changed = 1;
    p = strstr(p + to_len, from);
  }
  return changed;
}

static void NormalizeTextInPlace(char *s) {
  // Map common Unicode punctuation to ASCII equivalents expected by lexicons.
  (void)ReplaceAll(s, "\xE2\x80\x99", "'");  // ’
  (void)ReplaceAll(s, "\xE2\x80\x98", "'");  // ‘
  (void)ReplaceAll(s, "\xE2\x80\x9C", "\""); // “
  (void)ReplaceAll(s, "\xE2\x80\x9D", "\""); // ”
  (void)ReplaceAll(s, "\xE2\x80\x94", "-");  // —
  (void)ReplaceAll(s, "\xE2\x80\xA6", ".");  // …
  (void)ReplaceAll(s, "\xE2\x9D\x93", "?");  // ❓
  (void)ReplaceAll(s, "\xEF\xBC\x9F", "?");  // ？
  (void)ReplaceAll(s, "\xEF\xBC\x81", "!");  // ！
}

typedef struct PlaybackState {
  int32_t enabled;
  int32_t sample_rate;
#if defined(_WIN32)
  HWAVEOUT wave_out;
  int32_t wave_ready;
#endif
} PlaybackState;

#if defined(_WIN32)
static int InitPlayback(PlaybackState *s) {
  WAVEFORMATEX fmt;
  MMRESULT mmr;
  memset(&fmt, 0, sizeof(fmt));
  fmt.wFormatTag = WAVE_FORMAT_PCM;
  fmt.nChannels = 1;
  fmt.nSamplesPerSec = (DWORD)s->sample_rate;
  fmt.wBitsPerSample = 16;
  fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
  fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
  mmr = waveOutOpen(&s->wave_out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
  if (mmr != MMSYSERR_NOERROR) {
    fprintf(stderr, "Failed to open default audio output (winmm error %u)\n",
            (unsigned)mmr);
    return 0;
  }
  s->wave_ready = 1;
  return 1;
}

static void ClosePlayback(PlaybackState *s) {
  if (!s->wave_ready) return;
  waveOutReset(s->wave_out);
  waveOutClose(s->wave_out);
  s->wave_out = NULL;
  s->wave_ready = 0;
}
#endif

static int32_t ProgressCallbackWithArg(const float *samples, int32_t num_samples,
                                       float progress, void *arg) {
  PlaybackState *state = (PlaybackState *)arg;

  if (state && state->enabled) {
#if defined(_WIN32)
    if (num_samples > 0 && state->sample_rate > 0) {
      int32_t count = num_samples;
      short *pcm = (short *)malloc((size_t)count * sizeof(short));
      WAVEHDR hdr;
      int32_t i;

      if (pcm) {
        for (i = 0; i < count; ++i) {
          float x = samples[i];
          if (x > 1.0f) x = 1.0f;
          if (x < -1.0f) x = -1.0f;
          pcm[i] = (short)(x * 32767.0f);
        }

        memset(&hdr, 0, sizeof(hdr));
        hdr.lpData = (LPSTR)pcm;
        hdr.dwBufferLength = (DWORD)((size_t)count * sizeof(short));
        if (waveOutPrepareHeader(state->wave_out, &hdr, sizeof(hdr)) ==
                MMSYSERR_NOERROR &&
            waveOutWrite(state->wave_out, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) {
          while (!(hdr.dwFlags & WHDR_DONE)) {
            Sleep(5);
          }
          waveOutUnprepareHeader(state->wave_out, &hdr, sizeof(hdr));
        }
        free(pcm);
      }
    }
#else
    (void)samples;
    (void)num_samples;
#endif
  }

  (void)samples;
  (void)num_samples;
  fprintf(stderr, "Progress: %.3f%%\n", progress * 100);
  return 1;
}

int32_t main(int32_t argc, char *argv[]) {
  const char *model_dir = EVAL_KOKORO_MODEL_DIR;
  const char *text_file = EVAL_KOKORO_TEST_INPUT;
  const char *output_wav = EVAL_KOKORO_OUTPUT_WAV;
  int32_t sid = 0;
  float speed = 1.0f;
  int32_t debug = 1;
  int32_t playback = 1;
  int32_t include_zh_lexicon = 0;
  int32_t i;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--model-dir") == 0 && i + 1 < argc) {
      model_dir = argv[++i];
    } else if (strcmp(argv[i], "--text-file") == 0 && i + 1 < argc) {
      text_file = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output_wav = argv[++i];
    } else if (strcmp(argv[i], "--sid") == 0 && i + 1 < argc) {
      if (!ParseInt(argv[++i], &sid)) {
        fprintf(stderr, "Invalid --sid value\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &speed)) {
        fprintf(stderr, "Invalid --speed value\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--debug") == 0 && i + 1 < argc) {
      if (!ParseInt(argv[++i], &debug)) {
        fprintf(stderr, "Invalid --debug value\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--no-playback") == 0) {
      playback = 0;
    } else if (strcmp(argv[i], "--include-zh-lexicon") == 0) {
      include_zh_lexicon = 1;
    } else {
      fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
      PrintUsage(argv[0]);
      return 1;
    }
  }

  char model_path[4096];
  char voices_path[4096];
  char tokens_path[4096];
  char data_dir[4096];
  char dict_dir[4096];
  char lexicon_pair[8192];
  char *text = ReadEntireFile(text_file);

  if (!text) {
    fprintf(stderr, "Failed to read text file: %s\n", text_file);
    return 1;
  }
  NormalizeTextInPlace(text);

  snprintf(model_path, sizeof(model_path), "%s/model.onnx", model_dir);
  snprintf(voices_path, sizeof(voices_path), "%s/voices.bin", model_dir);
  snprintf(tokens_path, sizeof(tokens_path), "%s/tokens.txt", model_dir);
  snprintf(data_dir, sizeof(data_dir), "%s/espeak-ng-data", model_dir);
  snprintf(dict_dir, sizeof(dict_dir), "%s/dict", model_dir);
  if (include_zh_lexicon) {
    snprintf(lexicon_pair, sizeof(lexicon_pair),
             "%s/lexicon-us-en.txt,%s/lexicon-zh.txt", model_dir, model_dir);
  } else {
    snprintf(lexicon_pair, sizeof(lexicon_pair), "%s/lexicon-us-en.txt",
             model_dir);
  }

  SherpaOnnxOfflineTtsConfig config;
  memset(&config, 0, sizeof(config));
  config.model.kokoro.model = model_path;
  config.model.kokoro.voices = voices_path;
  config.model.kokoro.tokens = tokens_path;
  config.model.kokoro.data_dir = data_dir;
  config.model.kokoro.dict_dir = dict_dir;
  config.model.kokoro.lexicon = lexicon_pair;
  config.model.num_threads = 2;
  config.model.debug = debug;

  const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&config);
  if (!tts) {
    fprintf(stderr, "Failed to create Offline TTS.\n");
    free(text);
    return 1;
  }

  PlaybackState playback_state;
  memset(&playback_state, 0, sizeof(playback_state));
  playback_state.enabled = playback;
  playback_state.sample_rate = SherpaOnnxOfflineTtsSampleRate(tts);
#if defined(_WIN32)
  if (playback_state.enabled) {
    playback_state.enabled = InitPlayback(&playback_state);
  }
#endif

  const SherpaOnnxGeneratedAudio *audio =
      SherpaOnnxOfflineTtsGenerateWithProgressCallbackWithArg(
          tts, text, sid, speed, ProgressCallbackWithArg, &playback_state);
  if (!audio) {
    fprintf(stderr, "TTS generation failed.\n");
#if defined(_WIN32)
    ClosePlayback(&playback_state);
#endif
    SherpaOnnxDestroyOfflineTts(tts);
    free(text);
    return 1;
  }

  SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, output_wav);

#if defined(_WIN32)
  ClosePlayback(&playback_state);
#endif
  SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
  SherpaOnnxDestroyOfflineTts(tts);
  free(text);

  fprintf(stderr, "Input text file: %s\n", text_file);
  fprintf(stderr, "Model dir: %s\n", model_dir);
  fprintf(stderr, "Speaker ID is: %d\n", sid);
  fprintf(stderr, "Lexicon mode: %s\n",
          include_zh_lexicon ? "us-en + zh" : "us-en only");
  fprintf(stderr, "Playback: %s\n", playback_state.enabled ? "enabled" : "disabled");
  fprintf(stderr, "Saved to: %s\n", output_wav);

  return 0;
}
