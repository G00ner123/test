#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>

#define MAX_BARS        128     
#define MAX_WAVES       8
#define HUD_LINES       18
#define HUD_LINE_LEN    64

#define WIN_W_NORMAL    1920
#define WIN_H_NORMAL    1080

#define REFERENZ_DT     (1.0f / 60.0f)  
#define BARS_NORMAL     45
#define BARS_DENSE      80
#define BARS_HYPER      110

typedef struct {
    const char *name;
    SDL_Color   start;
    SDL_Color   end;
} Palette;

static const Palette PALETTES[] = {
    { "Cyberpunk",    {0xff, 0x00, 0x7f, 255}, {0x00, 0xe5, 0xff, 255} },
    { "Acid Matrix",  {0x00, 0xff, 0x66, 255}, {0x15, 0x6b, 0x35, 255} },
    { "Vaporwave",    {0x70, 0x00, 0xff, 255}, {0xff, 0x00, 0x55, 255} },
    { "Solar Flare",  {0xff, 0xa5, 0x00, 255}, {0xff, 0x2e, 0x00, 255} }, 
    { "Ocean Deep",   {0x00, 0xd9, 0xff, 255}, {0x00, 0x1f, 0x54, 255} }, 
};
#define PALETTE_COUNT ((int)(sizeof(PALETTES) / sizeof(PALETTES[0])))

typedef struct {
    SDL_Scancode key;
    float        percent;
} KeySegment;

static const KeySegment KEY_SEGMENTS[] = {
    { SDL_SCANCODE_Q, 0.05f }, { SDL_SCANCODE_A, 0.08f }, { SDL_SCANCODE_Y, 0.12f },
    { SDL_SCANCODE_W, 0.16f }, { SDL_SCANCODE_S, 0.20f }, { SDL_SCANCODE_X, 0.24f },
    { SDL_SCANCODE_E, 0.28f }, { SDL_SCANCODE_D, 0.32f }, { SDL_SCANCODE_C, 0.36f },
    { SDL_SCANCODE_R, 0.40f }, { SDL_SCANCODE_F, 0.44f }, { SDL_SCANCODE_V, 0.48f },
    { SDL_SCANCODE_T, 0.52f }, { SDL_SCANCODE_G, 0.56f }, { SDL_SCANCODE_B, 0.60f },
    { SDL_SCANCODE_Z, 0.64f }, { SDL_SCANCODE_H, 0.68f }, { SDL_SCANCODE_N, 0.72f },
    { SDL_SCANCODE_U, 0.76f }, { SDL_SCANCODE_J, 0.80f }, { SDL_SCANCODE_M, 0.84f },
    { SDL_SCANCODE_I, 0.88f }, { SDL_SCANCODE_K, 0.92f }, { SDL_SCANCODE_O, 0.95f },
    { SDL_SCANCODE_L, 0.98f }, { SDL_SCANCODE_P, 1.00f }, { SDL_SCANCODE_SPACE, 0.50f },
};
#define KEY_SEGMENT_COUNT ((int)(sizeof(KEY_SEGMENTS) / sizeof(KEY_SEGMENTS[0])))

typedef struct {
    float center;
    float progress;
    float power;
    bool  alive;
} Wave;


typedef enum {
    MODE_KEYBOARD = 0,
    MODE_MUSIC    = 1,
} InputMode;

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_CHUNK       512         
#define FFT_SIZE          2048        
#define FFT_BINS          (FFT_SIZE / 2)

typedef struct {
    SDL_Thread  *thread;
    SDL_mutex   *mutex;
    SDL_atomic_t running;      
    SDL_atomic_t available;    
    float        spectrum[FFT_BINS]; 
} AudioCapture;

static bool get_default_monitor_source(char *out, size_t out_len) {
    FILE *p = popen("pactl get-default-sink 2>/dev/null", "r");
    if (!p) return false;

    char sink_name[192];
    bool ok = (fgets(sink_name, sizeof(sink_name), p) != NULL);
    pclose(p);
    if (!ok) return false;

    size_t len = strlen(sink_name);
    while (len > 0 && (sink_name[len-1] == '\n' || sink_name[len-1] == '\r')) sink_name[--len] = '\0';
    if (len == 0) return false;

    snprintf(out, out_len, "%s.monitor", sink_name);
    return true;
}

static void fft_radix2(float *re, float *im, int n) {
    // Bit-Reversal-Permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    /* Schmetterlings-Stufen */
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cur_r = 1.0f, cur_i = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i+k],           ui = im[i+k];
                float vr = re[i+k+len/2] * cur_r - im[i+k+len/2] * cur_i;
                float vi = re[i+k+len/2] * cur_i + im[i+k+len/2] * cur_r;
                re[i+k]         = ur + vr;
                im[i+k]         = ui + vi;
                re[i+k+len/2]   = ur - vr;
                im[i+k+len/2]   = ui - vi;
                float next_r = cur_r * wr - cur_i * wi;
                float next_i = cur_r * wi + cur_i * wr;
                cur_r = next_r; cur_i = next_i;
            }
        }
    }
}

static int audio_thread_func(void *data) {
    AudioCapture *ac = (AudioCapture *)data;

    pa_sample_spec ss;
    ss.format   = PA_SAMPLE_FLOAT32NE;
    ss.rate     = AUDIO_SAMPLE_RATE;
    ss.channels = 1;

    char source_name[224];
    bool have_source = get_default_monitor_source(source_name, sizeof(source_name));

    int error = 0;
    pa_simple *pa = pa_simple_new(
        NULL, "visualizer_c", PA_STREAM_RECORD,
        have_source ? source_name : NULL,
        "system audio capture", &ss, NULL, NULL, &error);

    if (!pa) {
        fprintf(stderr,
            "[Musik-Modus] Konnte kein Audio erfassen (%s). "
            "Läuft PipeWire/PulseAudio? Musik-Modus zeigt dann keine Reaktion.\n",
            pa_strerror(error));
        SDL_AtomicSet(&ac->available, 0);
        return 0;
    }
    SDL_AtomicSet(&ac->available, 1);

    static float window_samples[FFT_SIZE];
    memset(window_samples, 0, sizeof(window_samples));
    float chunk[AUDIO_CHUNK];
    float re[FFT_SIZE], im[FFT_SIZE];

    while (SDL_AtomicGet(&ac->running)) {
        if (pa_simple_read(pa, chunk, sizeof(chunk), &error) < 0) {
            fprintf(stderr, "[Musik-Modus] Audio-Lesefehler: %s\n", pa_strerror(error));
            break;
        }

        memmove(window_samples, window_samples + AUDIO_CHUNK,
                (FFT_SIZE - AUDIO_CHUNK) * sizeof(float));
        memcpy(window_samples + (FFT_SIZE - AUDIO_CHUNK), chunk,
               AUDIO_CHUNK * sizeof(float));

        // Hann-Fenster anwenden (reduziert spektrales "Leaken")
        for (int i = 0; i < FFT_SIZE; i++) {
            float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
            re[i] = window_samples[i] * w;
            im[i] = 0.0f;
        }

        fft_radix2(re, im, FFT_SIZE);

        const float norm_factor = 1.0f / (FFT_SIZE / 2.0f);
        SDL_LockMutex(ac->mutex);
        for (int i = 0; i < FFT_BINS; i++) {
            ac->spectrum[i] = sqrtf(re[i] * re[i] + im[i] * im[i]) * norm_factor;
        }
        SDL_UnlockMutex(ac->mutex);
    }

    pa_simple_free(pa);
    return 0;
}

static void map_spectrum_to_band_avg(const float *spectrum, int num_bars, float *out_avg) {
    const float nyquist  = AUDIO_SAMPLE_RATE / 2.0f;
    const float bin_hz    = nyquist / FFT_BINS;
    const float min_freq  = 40.0f;
    const float max_freq  = fminf(16000.0f, nyquist - bin_hz);

    for (int b = 0; b < num_bars; b++) {
        float t0 = (float)b / (float)num_bars;
        float t1 = (float)(b + 1) / (float)num_bars;
        float f0 = min_freq * powf(max_freq / min_freq, t0);
        float f1 = min_freq * powf(max_freq / min_freq, t1);

        int bin0 = (int)(f0 / bin_hz);
        int bin1 = (int)(f1 / bin_hz);
        if (bin1 <= bin0) bin1 = bin0 + 1;
        if (bin0 < 0) bin0 = 0;
        if (bin1 > FFT_BINS) bin1 = FFT_BINS;

        float sum = 0.0f;
        int count = 0;
        for (int k = bin0; k < bin1; k++) { sum += spectrum[k]; count++; }
        out_avg[b] = (count > 0) ? (sum / count) : 0.0f;
    }
}


#define SETTINGS_SLIDER_COUNT 4

typedef struct {
    float sensitivity;      
    float height_scale;     
    float width_scale;      
    float music_smoothing;  
} VisualizerSettings;

typedef struct {
    const char *label;
    float min, max;
} SettingSliderDef;

static const SettingSliderDef SETTINGS_SLIDER_DEFS[SETTINGS_SLIDER_COUNT] = {
    { "EMPFINDLICHKEIT",    0.2f,  3.0f },
    { "BALKENHOEHE",        0.2f,  1.0f },
    { "BALKENBREITE",       0.3f,  2.0f },
    { "GLAETTUNG (MUSIK)",  0.05f, 0.6f },
};


typedef struct {
    /* Fenster / Renderer */
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *accum;          
    int           width, height;
    bool          hyper_mode;      
    int   current_palette;
    float speed_multiplier;
    bool  wave_fx_enabled;
    float gravity_mode;
    bool  premium_look;
    bool  shake_enabled;
    bool  glow_enabled;
    bool  dense_mode;
    bool  center_mirror;
    bool  background_freeze;
    bool  motion_blur;
    bool  glitch_mode;

    bool  show_fps;
    int   fps_cap;               

    InputMode    mode;
    AudioCapture audio;
    float        audio_bar_energy[MAX_BARS]; 
    float        bass_avg;                   
    float        beat_cooldown;              
    float        band_running_max[MAX_BARS]; 

    // Einstellungsmenü
    VisualizerSettings settings;
    bool         settings_panel_open;
    int          dragging_slider;           
    SDL_Texture *settings_label_tex[SETTINGS_SLIDER_COUNT];
    int          settings_label_w[SETTINGS_SLIDER_COUNT];
    int          settings_label_h[SETTINGS_SLIDER_COUNT];
    bool         settings_labels_ready;

    // Balken-Zustand 
    int   num_bars;
    float bar_heights[MAX_BARS];
    float target_heights[MAX_BARS];
    float flash_intensities[MAX_BARS];
    float seed_offsets[MAX_BARS];

    // Effekt-Zustand 
    float screen_shake;
    float sim_time;
    Wave  waves[MAX_WAVES];
    int   input_cooldown;
    float glitch_intensity;

    // HUD-Text-Cache 
    TTF_Font    *font;
    SDL_Texture *hud_textures[HUD_LINES];
    char         hud_cache[HUD_LINES][HUD_LINE_LEN];
    int          hud_tex_w[HUD_LINES], hud_tex_h[HUD_LINES];

    bool running;
} AppState;


static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static Uint8 clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (Uint8)v;
}

static float smooth_step(float value, float target, float rate_per_60hz_frame, float dt) {
    float scale = dt / REFERENZ_DT;
    float factor = 1.0f - powf(1.0f - rate_per_60hz_frame, scale);
    factor = clampf(factor, 0.0f, 1.0f);
    return value + (target - value) * factor;
}

static char *config_path(char *buf, size_t buflen) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(buf, buflen, "%s/.config/visualizer_c", home);
    return buf;
}


static void setup_bars(AppState *st) {
    if (st->hyper_mode)      st->num_bars = BARS_HYPER;
    else if (st->dense_mode) st->num_bars = BARS_DENSE;
    else                     st->num_bars = BARS_NORMAL;

    if (st->num_bars > MAX_BARS) st->num_bars = MAX_BARS; 

    for (int i = 0; i < st->num_bars; i++) {
        st->bar_heights[i]       = 130.0f;
        st->target_heights[i]    = 130.0f;
        st->flash_intensities[i] = 0.0f;
        st->seed_offsets[i]      = (float)(rand() % 10000) / 100.0f; /* 0..100 */
    }
}


static void destroy_accum(AppState *st) {
    if (st->accum) { SDL_DestroyTexture(st->accum); st->accum = NULL; }
}

static bool create_accum_texture(AppState *st) {
    destroy_accum(st);
    st->accum = SDL_CreateTexture(st->renderer, SDL_PIXELFORMAT_RGBA8888,
                                   SDL_TEXTUREACCESS_TARGET,
                                   st->width, st->height);
    if (!st->accum) {
        fprintf(stderr, "Konnte Accumulation-Textur nicht erstellen: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetTextureBlendMode(st->accum, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(st->renderer, st->accum);
    SDL_SetRenderDrawColor(st->renderer, 10, 10, 14, 255);
    SDL_RenderClear(st->renderer);
    SDL_SetRenderTarget(st->renderer, NULL);
    return true;
}

static void apply_vsync_for_refresh_rate(AppState *st) {
    int display_index = SDL_GetWindowDisplayIndex(st->window);
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(display_index, &mode) == 0 && mode.refresh_rate > 0) {
        st->fps_cap = mode.refresh_rate;
    } else {
        st->fps_cap = 120; /* Fallback-Zielwert */
    }
}

static void invalidate_hud_textures(AppState *st) {
    for (int i = 0; i < HUD_LINES; i++) {
        st->hud_textures[i] = NULL;  
        st->hud_cache[i][0] = '\0';   
    }
}

static bool rebuild_window(AppState *st) {
    if (st->window) {
        SDL_DestroyRenderer(st->renderer);   
        SDL_DestroyWindow(st->window);
        st->renderer = NULL;
        st->window = NULL;
        st->accum = NULL;                    
        invalidate_hud_textures(st);         
    }

    Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (st->hyper_mode) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    st->width  = st->hyper_mode ? 0 : WIN_W_NORMAL;  
    st->height = st->hyper_mode ? 0 : WIN_H_NORMAL;

    st->window = SDL_CreateWindow(
        "Hyprland Ultimate Cyber-Visualizer Pro (C/SDL2)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        st->hyper_mode ? WIN_W_NORMAL : WIN_W_NORMAL,  
        st->hyper_mode ? WIN_H_NORMAL : WIN_H_NORMAL,
        flags);
    if (!st->window) {
        fprintf(stderr, "SDL_CreateWindow fehlgeschlagen: %s\n", SDL_GetError());
        return false;
    }

    SDL_GetWindowSize(st->window, &st->width, &st->height);

    apply_vsync_for_refresh_rate(st);

    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    st->renderer = SDL_CreateRenderer(st->window, -1, renderer_flags);
    if (!st->renderer) {
        st->renderer = SDL_CreateRenderer(st->window, -1, SDL_RENDERER_ACCELERATED);
    }
    if (!st->renderer) {
        fprintf(stderr, "SDL_CreateRenderer fehlgeschlagen: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(st->renderer, SDL_BLENDMODE_BLEND);

    return create_accum_texture(st);
}

static void handle_window_resized(AppState *st, int new_w, int new_h) {
    st->width = new_w;
    st->height = new_h;
    create_accum_texture(st);
}


static void spawn_wave_if_slot_free(AppState *st, float center, float power);

static void update_bars(AppState *st, float dt) {
    float time_scale = dt / REFERENZ_DT;

    if (!st->background_freeze) {
        st->sim_time += 0.022f * st->speed_multiplier * time_scale;
    }
    if (st->input_cooldown > 0) st->input_cooldown--;

    if (st->glitch_intensity > 0.0f) {
        st->glitch_intensity *= powf(0.92f, time_scale);
    }

    if (st->screen_shake > 0.0f) {
        st->screen_shake *= powf(0.85f, time_scale);
        if (st->screen_shake < 0.5f) st->screen_shake = 0.0f;
    }

    /* --- Wellen (Wave-Impacts) --- */
    static float wave_impacts[MAX_BARS];
    memset(wave_impacts, 0, sizeof(float) * st->num_bars);

    float wave_step = (st->background_freeze ? 0.85f : 0.75f) * time_scale;

    for (int w = 0; w < MAX_WAVES; w++) {
        Wave *wave = &st->waves[w];
        if (!wave->alive) continue;

        float current_power = wave->power * fmaxf(0.0f, 1.0f - wave->progress / 22.0f);
        int left_idx  = (int)(wave->center - wave->progress);
        int right_idx = (int)(wave->center + wave->progress);

        if (left_idx >= 0 && left_idx < st->num_bars && current_power > 0.01f)
            wave_impacts[left_idx] += current_power;
        if (right_idx >= 0 && right_idx < st->num_bars && current_power > 0.01f)
            wave_impacts[right_idx] += current_power;

        wave->progress += wave_step;

        if (current_power <= 0.0f || (left_idx < 0 && right_idx >= st->num_bars)) {
            wave->alive = false;
        }
    }

    if (st->mode == MODE_MUSIC) {
        SDL_LockMutex(st->audio.mutex);
        float spectrum_copy[FFT_BINS];
        memcpy(spectrum_copy, st->audio.spectrum, sizeof(spectrum_copy));
        SDL_UnlockMutex(st->audio.mutex);

        float raw_avg[MAX_BARS];
        map_spectrum_to_band_avg(spectrum_copy, st->num_bars, raw_avg);

        float decay = powf(0.5f, dt / 1.2f);
        for (int i = 0; i < st->num_bars; i++) {
            st->band_running_max[i] *= decay;
            if (raw_avg[i] > st->band_running_max[i]) st->band_running_max[i] = raw_avg[i];
            if (st->band_running_max[i] < 0.0001f)    st->band_running_max[i] = 0.0001f;

            float norm = (raw_avg[i] / st->band_running_max[i]) * st->settings.sensitivity;
            st->audio_bar_energy[i] = clampf(norm, 0.0f, 1.0f);
        }

        int bass_bars = st->num_bars / 7;
        if (bass_bars < 1) bass_bars = 1;
        float bass_now = 0.0f;
        for (int i = 0; i < bass_bars; i++) bass_now += st->audio_bar_energy[i];
        bass_now /= (float)bass_bars;

        st->bass_avg += (bass_now - st->bass_avg) * clampf(0.05f * time_scale, 0.0f, 1.0f);
        st->beat_cooldown -= dt;

        if (bass_now > 0.32f && bass_now > st->bass_avg * 1.5f && st->beat_cooldown <= 0.0f) {
            spawn_wave_if_slot_free(st, (float)st->num_bars / 2.0f, st->height * 0.8f);
            st->beat_cooldown = 0.18f; 
        }
    }

    for (int i = 0; i < st->num_bars; i++) {
        float target;
        if (st->mode == MODE_MUSIC) {
            
            float max_h = (st->height - 20.0f) * st->settings.height_scale;
            target = 10.0f + st->audio_bar_energy[i] * (max_h - 10.0f);
        } else if (st->background_freeze) {
            target = 10.0f;
        } else {
            float neighbor_influence = sinf(st->sim_time + (i / 3) * 1.1f) * 35.0f;
            float individual_noise   = sinf(st->sim_time * 1.6f + st->seed_offsets[i]) * 40.0f;
            float fast_twitch        = cosf(st->sim_time * 3.5f + st->seed_offsets[i] * 1.2f) * 10.0f;
            float base_idle = st->center_mirror ? 70.0f : 130.0f;
            target = fmaxf(15.0f, base_idle + neighbor_influence + individual_noise + fast_twitch);
            target *= st->settings.height_scale;
        }

        if (st->glitch_mode && st->glitch_intensity > 0.1f) {
            float glitch_modifier = sinf(i * 0.8f + st->sim_time * 5.0f) *
                                     (st->height * 0.3f) * st->glitch_intensity;
            target += glitch_modifier;
        }

        if (st->wave_fx_enabled) target += wave_impacts[i];

        if (st->mode == MODE_MUSIC) {
            st->target_heights[i] = target;
            st->bar_heights[i] = smooth_step(st->bar_heights[i], target,
                                              st->settings.music_smoothing, dt);
        } else {
            st->target_heights[i] = smooth_step(st->target_heights[i], target, 0.12f, dt);
            st->bar_heights[i]    = smooth_step(st->bar_heights[i], st->target_heights[i],
                                                 st->gravity_mode, dt);
        }

        if (st->flash_intensities[i] > 0.0f) {
            st->flash_intensities[i] -= 0.05f * time_scale;
            if (st->flash_intensities[i] < 0.0f) st->flash_intensities[i] = 0.0f;
        }
    }
}


static void spawn_wave_if_slot_free(AppState *st, float center, float power) {
    for (int w = 0; w < MAX_WAVES; w++) {
        if (!st->waves[w].alive) {
            st->waves[w].alive    = true;
            st->waves[w].center   = center;
            st->waves[w].progress = 0.0f;
            st->waves[w].power    = power;
            return;
        }
    }

}

static void handle_fast_input(AppState *st, const Uint8 *keystate) {
    if (st->input_cooldown > 0) return;

    int pressed_positions[KEY_SEGMENT_COUNT];
    int pressed_count = 0;

    for (int k = 0; k < KEY_SEGMENT_COUNT; k++) {
        if (keystate[KEY_SEGMENTS[k].key]) {
            int idx = (int)(st->num_bars * KEY_SEGMENTS[k].percent);
            pressed_positions[pressed_count++] = idx;
        }
    }

    if (pressed_count > 0) {
        st->input_cooldown = 2;
        st->glitch_intensity = fminf(1.0f, st->glitch_intensity + 0.35f);
    }

    for (int p = 0; p < pressed_count; p++) {
        int center_idx = pressed_positions[p];
        if (center_idx < 0) center_idx = 0;
        if (center_idx > st->num_bars - 1) center_idx = st->num_bars - 1;

        if (st->shake_enabled && st->screen_shake < 5.0f) st->screen_shake = 7.5f;

        if (st->wave_fx_enabled) {
            float power_factor = st->background_freeze ? st->height * 1.1f : st->height * 0.65f;
            spawn_wave_if_slot_free(st, (float)center_idx, power_factor);
        }

        for (int offset = -5; offset <= 5; offset++) {
            int idx = center_idx + offset;
            if (idx < 0 || idx >= st->num_bars) continue;

            float distance_factor = (6.0f - fabsf((float)offset)) / 6.0f;
            float max_h = (st->center_mirror ? (st->height / 2.0f - 20.0f) : (st->height - 20.0f))
                          * st->settings.height_scale;
            float boost = st->background_freeze ? 110.0f : 60.0f;
            float impact = (max_h + boost) * distance_factor;

            if (impact > st->bar_heights[idx]) {
                st->bar_heights[idx] = fminf(max_h, impact);
                st->flash_intensities[idx] = distance_factor * 0.25f;
            }
        }
    }
}


static void update_hud_line(AppState *st, int line_index, const char *text) {
    if (strncmp(st->hud_cache[line_index], text, HUD_LINE_LEN) == 0 &&
        st->hud_textures[line_index] != NULL) {
        return; /* Unverändert - kein Neu-Rendern nötig */
    }

    if (st->hud_textures[line_index]) {
        SDL_DestroyTexture(st->hud_textures[line_index]);
        st->hud_textures[line_index] = NULL;
    }

    SDL_Color color = {180, 180, 220, 255};
    SDL_Surface *surf = TTF_RenderText_Blended(st->font, text, color);
    if (!surf) return;

    st->hud_textures[line_index] = SDL_CreateTextureFromSurface(st->renderer, surf);
    st->hud_tex_w[line_index] = surf->w;
    st->hud_tex_h[line_index] = surf->h;
    SDL_FreeSurface(surf);

    snprintf(st->hud_cache[line_index], HUD_LINE_LEN, "%s", text);
}

static void draw_hud(AppState *st, float current_fps) {
    if (!st->font) return;

    char lines[HUD_LINES][HUD_LINE_LEN];
    const char *bars_txt = st->hyper_mode ? "110 HYPER-LINES" : (st->dense_mode ? "80 LINES" : "45 BLOCKS");

    snprintf(lines[0],  HUD_LINE_LEN, " 1 THEME:  %s", PALETTES[st->current_palette].name);
    snprintf(lines[1],  HUD_LINE_LEN, " 2 SPEED:  %s", st->speed_multiplier > 1.0f ? "FAST" : "NORMAL");
    snprintf(lines[2],  HUD_LINE_LEN, " 3 WAVE:   %s", st->wave_fx_enabled ? "ON" : "OFF");
    snprintf(lines[3],  HUD_LINE_LEN, " 4 PHYS:   %s", st->gravity_mode < 0.1f ? "FLOAT" : "SNAPPY");
    snprintf(lines[4],  HUD_LINE_LEN, " 5 LOOK:   %s", st->premium_look ? "PREMIUM" : "CLASSIC");
    snprintf(lines[5],  HUD_LINE_LEN, " 6 SHAKE:  %s", st->shake_enabled ? "ON" : "OFF");
    snprintf(lines[6],  HUD_LINE_LEN, " 7 GLOW:   %s", st->glow_enabled ? "3D-SHADED" : "OFF");
    snprintf(lines[7],  HUD_LINE_LEN, " 8 BARS:   %s", bars_txt);
    snprintf(lines[8],  HUD_LINE_LEN, " 9 COMP:   %s", st->center_mirror ? "CENTER-OSC" : "BOTTOM");
    snprintf(lines[9],  HUD_LINE_LEN, " 0 MODE:   %s", st->background_freeze ? "KEY-ONLY" : "AUTO-FLOW");
    snprintf(lines[10], HUD_LINE_LEN, " [BACK]    %s", st->hyper_mode ? "FULLSCREEN 4K (ECHT)" : "WINDOW (1920px)");
    snprintf(lines[11], HUD_LINE_LEN, " [-] TRAIL:%s", st->motion_blur ? "ON (BLUR)" : "OFF");
    snprintf(lines[12], HUD_LINE_LEN, " [#] FX:   %s", st->glitch_mode ? "CHAOS-GLITCH" : "OFF");
    snprintf(lines[13], HUD_LINE_LEN, " [F1] FPS: %s", st->show_fps ? "SICHTBAR" : "AUS");
    snprintf(lines[14], HUD_LINE_LEN, " [F2/F3]   SAVE / LOAD CONFIG");
    if (st->fps_cap > 0) snprintf(lines[15], HUD_LINE_LEN, " [F4] CAP: %d Hz", st->fps_cap);
    else                 snprintf(lines[15], HUD_LINE_LEN, " [F4] CAP: UNBEGRENZT");

    if (st->mode == MODE_MUSIC) {
        bool audio_ok = SDL_AtomicGet(&st->audio.available) != 0;
        snprintf(lines[16], HUD_LINE_LEN, " [ENTER]   MODE: MUSIK%s",
                 audio_ok ? "" : " (KEIN AUDIO!)");
    } else {
        snprintf(lines[16], HUD_LINE_LEN, " [ENTER]   MODE: TASTATUR");
    }

    if (st->show_fps) {
        snprintf(lines[17], HUD_LINE_LEN, " FPS: %.1f", current_fps);
    } else {
        lines[17][0] = '\0';
    }

    int visible_lines = st->show_fps ? HUD_LINES : HUD_LINES - 1;

    for (int i = 0; i < visible_lines; i++) {
        update_hud_line(st, i, lines[i]);
        if (!st->hud_textures[i]) continue;

        SDL_Rect bg = { 8, 8 + i * 14, 175, 13 };
        SDL_SetRenderDrawColor(st->renderer, 0, 0, 0, 140);
        SDL_SetRenderDrawBlendMode(st->renderer, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(st->renderer, &bg);

        SDL_Rect dst = { 12, 8 + i * 14, st->hud_tex_w[i], st->hud_tex_h[i] };
        SDL_RenderCopy(st->renderer, st->hud_textures[i], NULL, &dst);
    }
}


static int hud_visible_line_count(const AppState *st) {
    return st->show_fps ? HUD_LINES : HUD_LINES - 1;
}

static SDL_Rect get_gear_button_rect(const AppState *st) {
    SDL_Rect r;
    r.x = 8;
    r.y = 8 + hud_visible_line_count(st) * 14 + 8;
    r.w = 22;
    r.h = 22;
    return r;
}

static SDL_Rect get_settings_panel_rect(const AppState *st) {
    SDL_Rect gear = get_gear_button_rect(st);
    SDL_Rect r;
    r.x = gear.x;
    r.y = gear.y + gear.h + 6;
    r.w = 230;
    r.h = 14 + SETTINGS_SLIDER_COUNT * 38;
    return r;
}

static SDL_Rect get_slider_track_rect(const AppState *st, int i) {
    SDL_Rect panel = get_settings_panel_rect(st);
    SDL_Rect r;
    r.x = panel.x + 12;
    r.y = panel.y + 14 + i * 38 + 18;
    r.w = panel.w - 24;
    r.h = 6;
    return r;
}

static float *get_slider_value_ptr(AppState *st, int i) {
    switch (i) {
        case 0: return &st->settings.sensitivity;
        case 1: return &st->settings.height_scale;
        case 2: return &st->settings.width_scale;
        case 3: return &st->settings.music_smoothing;
        default: return NULL;
    }
}

static void draw_filled_circle(SDL_Renderer *r, int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrtf((float)(radius * radius - dy * dy));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void draw_gear_icon(SDL_Renderer *r, SDL_Rect rect, SDL_Color color) {
    int cx = rect.x + rect.w / 2;
    int cy = rect.y + rect.h / 2;
    int outer_r = rect.w / 2 - 1;
    int inner_r = outer_r / 2;

    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, 230);
    draw_filled_circle(r, cx, cy, outer_r);

    const int teeth = 8;
    for (int i = 0; i < teeth; i++) {
        float angle = (2.0f * (float)M_PI / teeth) * (float)i;
        int tx = cx + (int)(cosf(angle) * outer_r);
        int ty = cy + (int)(sinf(angle) * outer_r);
        SDL_Rect tooth = { tx - 2, ty - 2, 4, 4 };
        SDL_RenderFillRect(r, &tooth);
    }

    SDL_SetRenderDrawColor(r, 10, 10, 14, 255);
    draw_filled_circle(r, cx, cy, inner_r);
}

static void ensure_settings_labels(AppState *st) {
    if (st->settings_labels_ready || !st->font) return;
    SDL_Color color = {210, 210, 230, 255};
    for (int i = 0; i < SETTINGS_SLIDER_COUNT; i++) {
        SDL_Surface *surf = TTF_RenderText_Blended(st->font, SETTINGS_SLIDER_DEFS[i].label, color);
        if (!surf) continue;
        st->settings_label_tex[i] = SDL_CreateTextureFromSurface(st->renderer, surf);
        st->settings_label_w[i] = surf->w;
        st->settings_label_h[i] = surf->h;
        SDL_FreeSurface(surf);
    }
    st->settings_labels_ready = true;
}

static void draw_settings_ui(AppState *st) {
    SDL_Rect gear = get_gear_button_rect(st);

    SDL_SetRenderDrawBlendMode(st->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(st->renderer, 0, 0, 0, 140);
    SDL_Rect gear_bg = { gear.x - 3, gear.y - 3, gear.w + 6, gear.h + 6 };
    SDL_RenderFillRect(st->renderer, &gear_bg);

    SDL_Color icon_color = st->settings_panel_open
        ? (SDL_Color){235, 235, 245, 255}
        : (SDL_Color){160, 160, 195, 255};
    draw_gear_icon(st->renderer, gear, icon_color);

    if (!st->settings_panel_open) return;

    ensure_settings_labels(st);

    SDL_Rect panel = get_settings_panel_rect(st);
    SDL_SetRenderDrawColor(st->renderer, 15, 15, 20, 235);
    SDL_RenderFillRect(st->renderer, &panel);
    SDL_SetRenderDrawColor(st->renderer, 90, 90, 115, 255);
    SDL_RenderDrawRect(st->renderer, &panel);

    for (int i = 0; i < SETTINGS_SLIDER_COUNT; i++) {
        float *value = get_slider_value_ptr(st, i);
        const SettingSliderDef *def = &SETTINGS_SLIDER_DEFS[i];
        SDL_Rect track = get_slider_track_rect(st, i);

        if (st->settings_label_tex[i]) {
            SDL_Rect label_dst = { track.x, track.y - 14, st->settings_label_w[i], st->settings_label_h[i] };
            SDL_RenderCopy(st->renderer, st->settings_label_tex[i], NULL, &label_dst);
        }

        char value_text[32];
        snprintf(value_text, sizeof(value_text), "%.2f", *value);
        SDL_Surface *vsurf = TTF_RenderText_Blended(st->font, value_text, (SDL_Color){235, 235, 245, 255});
        if (vsurf) {
            SDL_Texture *vtex = SDL_CreateTextureFromSurface(st->renderer, vsurf);
            SDL_Rect vdst = { panel.x + panel.w - 12 - vsurf->w, track.y - 14, vsurf->w, vsurf->h };
            SDL_RenderCopy(st->renderer, vtex, NULL, &vdst);
            SDL_DestroyTexture(vtex);
            SDL_FreeSurface(vsurf);
        }

        SDL_SetRenderDrawColor(st->renderer, 60, 60, 78, 255);
        SDL_RenderFillRect(st->renderer, &track);

        float t = clampf((*value - def->min) / (def->max - def->min), 0.0f, 1.0f);
        SDL_Rect fill = track;
        fill.w = (int)(track.w * t);
        SDL_SetRenderDrawColor(st->renderer, 170, 170, 220, 255);
        SDL_RenderFillRect(st->renderer, &fill);

        SDL_Rect handle = { track.x + fill.w - 3, track.y - 4, 6, track.h + 8 };
        SDL_SetRenderDrawColor(st->renderer, 235, 235, 245, 255);
        SDL_RenderFillRect(st->renderer, &handle);
    }
}

static void handle_mouse_down(AppState *st, int mx, int my) {
    SDL_Point p = { mx, my };

    SDL_Rect gear = get_gear_button_rect(st);
    SDL_Rect gear_hit = { gear.x - 3, gear.y - 3, gear.w + 6, gear.h + 6 };
    if (SDL_PointInRect(&p, &gear_hit)) {
        st->settings_panel_open = !st->settings_panel_open;
        st->dragging_slider = -1;
        return;
    }

    if (!st->settings_panel_open) return;

    SDL_Rect panel = get_settings_panel_rect(st);
    if (!SDL_PointInRect(&p, &panel)) {
        st->settings_panel_open = false;
        return;
    }

    for (int i = 0; i < SETTINGS_SLIDER_COUNT; i++) {
        SDL_Rect track = get_slider_track_rect(st, i);
        SDL_Rect hit = { track.x, track.y - 10, track.w, track.h + 20 }; /* großzügige Klickfläche */
        if (SDL_PointInRect(&p, &hit)) {
            st->dragging_slider = i;
            float t = clampf((float)(mx - track.x) / (float)track.w, 0.0f, 1.0f);
            float *value = get_slider_value_ptr(st, i);
            const SettingSliderDef *def = &SETTINGS_SLIDER_DEFS[i];
            *value = def->min + t * (def->max - def->min);
            return;
        }
    }
}

static void handle_mouse_motion(AppState *st, int mx) {
    if (st->dragging_slider < 0) return;
    SDL_Rect track = get_slider_track_rect(st, st->dragging_slider);
    float t = clampf((float)(mx - track.x) / (float)track.w, 0.0f, 1.0f);
    float *value = get_slider_value_ptr(st, st->dragging_slider);
    const SettingSliderDef *def = &SETTINGS_SLIDER_DEFS[st->dragging_slider];
    *value = def->min + t * (def->max - def->min);
}

static void handle_mouse_up(AppState *st) {
    st->dragging_slider = -1;
}


static void render_frame(AppState *st, float current_fps) {
    /* --- Schritt 1: In den Accumulation-Buffer rendern --- */
    SDL_SetRenderTarget(st->renderer, st->accum);

    if (st->motion_blur) {
        SDL_SetRenderDrawBlendMode(st->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(st->renderer, 10, 10, 14, 25);
        SDL_Rect full = {0, 0, st->width, st->height};
        SDL_RenderFillRect(st->renderer, &full);
    } else {
        SDL_SetRenderDrawBlendMode(st->renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(st->renderer, 10, 10, 14, 255);
        SDL_RenderClear(st->renderer);
    }

    float total_spacing_width = st->width * 0.08f;
    float available_width = st->width - total_spacing_width;
    float bar_width = available_width / st->num_bars;
    float spacing = total_spacing_width / (st->num_bars + 1);

    SDL_Color c_start = PALETTES[st->current_palette].start;
    SDL_Color c_end   = PALETTES[st->current_palette].end;

    SDL_SetRenderDrawBlendMode(st->renderer, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < st->num_bars; i++) {
        float height_val = st->bar_heights[i];
        if (height_val <= 4.0f) continue;

        float x = spacing + i * (bar_width + spacing);
        float t_boosted = (st->num_bars > 1)
            ? sinf(((float)i / (st->num_bars - 1)) * (float)M_PI / 2.0f)
            : 0.0f;

        Uint8 cr, cg, cb;
        if (st->mode == MODE_MUSIC) {
            float e = clampf(st->audio_bar_energy[i], 0.0f, 1.0f);
            SDL_Color beige = {232, 220, 196, 255};
            SDL_Color white = {255, 255, 255, 255};
            cr = clamp_u8((int)(beige.r + e * (white.r - beige.r)));
            cg = clamp_u8((int)(beige.g + e * (white.g - beige.g)));
            cb = clamp_u8((int)(beige.b + e * (white.b - beige.b)));
        } else {
            int r = (int)(c_start.r + t_boosted * (c_end.r - c_start.r) + (255 * st->flash_intensities[i] * 0.15f));
            int g = (int)(c_start.g + t_boosted * (c_end.g - c_start.g) + (255 * st->flash_intensities[i] * 0.15f));
            int b = (int)(c_start.b + t_boosted * (c_end.b - c_start.b) + (255 * st->flash_intensities[i] * 0.15f));
            cr = clamp_u8(r); cg = clamp_u8(g); cb = clamp_u8(b);
        }

        float draw_y = st->center_mirror ? (st->height / 2.0f - height_val) : (st->height - height_val);
        float draw_h = st->center_mirror ? height_val * 2.0f : height_val;

        float draw_width = bar_width * st->settings.width_scale;
        float x_centered = x + (bar_width - draw_width) / 2.0f;

        if (st->glow_enabled) {
            float glow_w = draw_width * 4.5f;
            SDL_Rect glow_rect = {
                (int)(x_centered - (glow_w - draw_width) / 2.0f),
                (int)draw_y, (int)glow_w, (int)draw_h
            };
            SDL_SetRenderDrawColor(st->renderer, c_start.r, c_start.g, c_start.b, 32);
            SDL_RenderFillRect(st->renderer, &glow_rect);
        }

        SDL_Rect bar_rect = { (int)x_centered, (int)draw_y, (int)ceilf(draw_width), (int)draw_h };

        if (st->premium_look) {
            SDL_SetRenderDrawColor(st->renderer, clamp_u8((int)(cr*0.2f)), clamp_u8((int)(cg*0.2f)), clamp_u8((int)(cb*0.2f)), 255);
            SDL_RenderFillRect(st->renderer, &bar_rect);
            SDL_SetRenderDrawColor(st->renderer, cr, cg, cb, 255);
            SDL_RenderDrawRect(st->renderer, &bar_rect);
        } else if (st->glow_enabled) {
            SDL_SetRenderDrawColor(st->renderer, cr, cg, cb, 255);
            SDL_RenderFillRect(st->renderer, &bar_rect);
            SDL_Rect highlight = { (int)(x_centered + draw_width * 0.25f), (int)draw_y, (int)(draw_width * 0.5f), (int)draw_h };
            SDL_SetRenderDrawColor(st->renderer, clamp_u8((int)(cr*1.3f)), clamp_u8((int)(cg*1.3f)), clamp_u8((int)(cb*1.3f)), 255);
            SDL_RenderFillRect(st->renderer, &highlight);
            SDL_SetRenderDrawColor(st->renderer, clamp_u8((int)(cr*0.4f)), clamp_u8((int)(cg*0.4f)), clamp_u8((int)(cb*0.4f)), 255);
            SDL_RenderDrawRect(st->renderer, &bar_rect);
        } else {
            SDL_SetRenderDrawColor(st->renderer, cr, cg, cb, 255);
            SDL_RenderFillRect(st->renderer, &bar_rect);
        }
    }

    if (st->hyper_mode) {
        SDL_SetRenderDrawColor(st->renderer, c_start.r, c_start.g, c_start.b, 255);
        SDL_Rect border = {0, 0, st->width, st->height};
        SDL_RenderDrawRect(st->renderer, &border);
    }

    SDL_SetRenderTarget(st->renderer, NULL);
    SDL_SetRenderDrawColor(st->renderer, 10, 10, 14, 255);
    SDL_RenderClear(st->renderer);

    int shift_x = 0, shift_y = 0;
    if (st->mode == MODE_KEYBOARD && st->shake_enabled && st->screen_shake > 0.0f) {
        shift_x = (rand() % (int)(2 * st->screen_shake + 1)) - (int)st->screen_shake;
        shift_y = (rand() % (int)(2 * st->screen_shake + 1)) - (int)st->screen_shake;
    }
    SDL_Rect dst = { shift_x, shift_y, st->width, st->height };
    SDL_RenderCopy(st->renderer, st->accum, NULL, &dst);

    draw_hud(st, current_fps);
    draw_settings_ui(st);

    SDL_RenderPresent(st->renderer);
}


#include <sys/stat.h>
#include <sys/types.h>

static void save_config(AppState *st) {
    char dir[512], path[600];
    config_path(dir, sizeof(dir));

    mkdir(dir, 0755);

    snprintf(path, sizeof(path), "%s/config.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Konnte Konfiguration nicht speichern: %s\n", path);
        return;
    }

    fprintf(f, "palette=%d\n",           st->current_palette);
    fprintf(f, "speed_multiplier=%f\n",  st->speed_multiplier);
    fprintf(f, "wave_fx=%d\n",           st->wave_fx_enabled);
    fprintf(f, "gravity=%f\n",           st->gravity_mode);
    fprintf(f, "premium_look=%d\n",      st->premium_look);
    fprintf(f, "shake=%d\n",             st->shake_enabled);
    fprintf(f, "glow=%d\n",              st->glow_enabled);
    fprintf(f, "dense_mode=%d\n",        st->dense_mode);
    fprintf(f, "center_mirror=%d\n",     st->center_mirror);
    fprintf(f, "background_freeze=%d\n", st->background_freeze);
    fprintf(f, "motion_blur=%d\n",       st->motion_blur);
    fprintf(f, "glitch_mode=%d\n",       st->glitch_mode);
    fprintf(f, "fps_cap=%d\n",           st->fps_cap);
    fprintf(f, "sensitivity=%f\n",       st->settings.sensitivity);
    fprintf(f, "height_scale=%f\n",      st->settings.height_scale);
    fprintf(f, "width_scale=%f\n",       st->settings.width_scale);
    fprintf(f, "music_smoothing=%f\n",   st->settings.music_smoothing);

    fclose(f);
}

static void load_config(AppState *st) {
    char dir[512], path[600];
    config_path(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/config.txt", dir);

    FILE *f = fopen(path, "r");
    if (!f) return; 

    char key[64];
    char value[64];
    while (fscanf(f, "%63[^=]=%63s\n", key, value) == 2) {
        if      (strcmp(key, "palette") == 0)           st->current_palette   = atoi(value) % PALETTE_COUNT;
        else if (strcmp(key, "speed_multiplier") == 0)  st->speed_multiplier  = atof(value);
        else if (strcmp(key, "wave_fx") == 0)           st->wave_fx_enabled   = atoi(value) != 0;
        else if (strcmp(key, "gravity") == 0)           st->gravity_mode      = atof(value);
        else if (strcmp(key, "premium_look") == 0)      st->premium_look      = atoi(value) != 0;
        else if (strcmp(key, "shake") == 0)             st->shake_enabled     = atoi(value) != 0;
        else if (strcmp(key, "glow") == 0)              st->glow_enabled      = atoi(value) != 0;
        else if (strcmp(key, "dense_mode") == 0)        st->dense_mode        = atoi(value) != 0;
        else if (strcmp(key, "center_mirror") == 0)     st->center_mirror     = atoi(value) != 0;
        else if (strcmp(key, "background_freeze") == 0) st->background_freeze = atoi(value) != 0;
        else if (strcmp(key, "motion_blur") == 0)       st->motion_blur       = atoi(value) != 0;
        else if (strcmp(key, "glitch_mode") == 0)       st->glitch_mode       = atoi(value) != 0;
        else if (strcmp(key, "fps_cap") == 0)            st->fps_cap          = atoi(value);
        else if (strcmp(key, "sensitivity") == 0)        st->settings.sensitivity     = atof(value);
        else if (strcmp(key, "height_scale") == 0)       st->settings.height_scale    = atof(value);
        else if (strcmp(key, "width_scale") == 0)        st->settings.width_scale     = atof(value);
        else if (strcmp(key, "music_smoothing") == 0)    st->settings.music_smoothing = atof(value);
    }

    fclose(f);
    setup_bars(st); 
}


static bool init_state(AppState *st) {
    memset(st, 0, sizeof(*st));

    st->current_palette   = 0;
    st->speed_multiplier  = 1.0f;
    st->wave_fx_enabled   = true;
    st->gravity_mode      = 0.16f;
    st->premium_look      = false;
    st->shake_enabled     = false;
    st->glow_enabled      = false;
    st->dense_mode        = false;
    st->center_mirror     = false;
    st->background_freeze = false;
    st->hyper_mode        = false;
    st->motion_blur       = false;
    st->glitch_mode       = false;
    st->show_fps          = false;
    st->fps_cap           = 120;
    st->mode              = MODE_KEYBOARD;
    st->bass_avg          = 0.0f;
    st->beat_cooldown     = 0.0f;
    for (int i = 0; i < MAX_BARS; i++) st->band_running_max[i] = 0.0001f;

    st->settings.sensitivity     = 1.0f;
    st->settings.height_scale    = 1.0f;
    st->settings.width_scale     = 1.0f;
    st->settings.music_smoothing = 0.22f;
    st->settings_panel_open      = false;
    st->dragging_slider          = -1;
    st->settings_labels_ready    = false;
    st->screen_shake      = 0.0f;
    st->sim_time          = 0.0f;
    st->input_cooldown    = 0;
    st->glitch_intensity  = 0.0f;
    st->running           = true;

    for (int i = 0; i < MAX_WAVES; i++) st->waves[i].alive = false;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init fehlgeschlagen: %s\n", SDL_GetError());
        return false;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init fehlgeschlagen: %s\n", TTF_GetError());
        return false;
    }

    if (!rebuild_window(st)) return false;
    setup_bars(st);

    const char *font_candidates[] = {
        "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",              /* Arch/CachyOS */
        "/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",  /* Debian/Ubuntu */
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        NULL
    };
    for (int i = 0; font_candidates[i] != NULL; i++) {
        st->font = TTF_OpenFont(font_candidates[i], 12);
        if (st->font) break;
    }
    if (!st->font) {
        fprintf(stderr, "Warnung: Keine Monospace-Schrift gefunden - HUD bleibt leer.\n"
                         "Installiere z.B. mit: sudo pacman -S ttf-dejavu\n");
    }

    st->audio.mutex = SDL_CreateMutex();
    SDL_AtomicSet(&st->audio.running, 1);
    SDL_AtomicSet(&st->audio.available, 0);
    memset(st->audio.spectrum, 0, sizeof(st->audio.spectrum));
    st->audio.thread = SDL_CreateThread(audio_thread_func, "audio_capture", &st->audio);
    if (!st->audio.thread) {
        fprintf(stderr, "Warnung: Audio-Thread konnte nicht gestartet werden: %s\n", SDL_GetError());
    }

    return true;
}

static void shutdown_state(AppState *st) {
    if (st->audio.thread) {
        SDL_AtomicSet(&st->audio.running, 0);
        SDL_WaitThread(st->audio.thread, NULL);
    }
    if (st->audio.mutex) SDL_DestroyMutex(st->audio.mutex);

    for (int i = 0; i < HUD_LINES; i++) {
        if (st->hud_textures[i]) SDL_DestroyTexture(st->hud_textures[i]);
    }
    for (int i = 0; i < SETTINGS_SLIDER_COUNT; i++) {
        if (st->settings_label_tex[i]) SDL_DestroyTexture(st->settings_label_tex[i]);
    }
    if (st->font) TTF_CloseFont(st->font);
    destroy_accum(st);
    if (st->renderer) SDL_DestroyRenderer(st->renderer);
    if (st->window) SDL_DestroyWindow(st->window);
    TTF_Quit();
    SDL_Quit();
}


static void handle_keydown(AppState *st, SDL_Keycode key) {
    switch (key) {
        case SDLK_ESCAPE:
            if (st->settings_panel_open) st->settings_panel_open = false;
            else                          st->running = false;
            break;
        case SDLK_1:
            st->current_palette = (st->current_palette + 1) % PALETTE_COUNT;
            break;
        case SDLK_2:
            st->speed_multiplier = (st->speed_multiplier == 1.0f) ? 2.2f : 1.0f;
            break;
        case SDLK_3:
            st->wave_fx_enabled = !st->wave_fx_enabled;
            break;
        case SDLK_4:
            st->gravity_mode = (st->gravity_mode == 0.16f) ? 0.05f : 0.16f;
            break;
        case SDLK_5:
            st->premium_look = !st->premium_look;
            break;
        case SDLK_6:
            st->shake_enabled = !st->shake_enabled;
            break;
        case SDLK_7:
            st->glow_enabled = !st->glow_enabled;
            break;
        case SDLK_8:
            if (!st->hyper_mode) {
                for (int i = 0; i < MAX_WAVES; i++) st->waves[i].alive = false;
                st->dense_mode = !st->dense_mode;
                setup_bars(st);
            }
            break;
        case SDLK_9:
            st->center_mirror = !st->center_mirror;
            break;
        case SDLK_0:
            st->background_freeze = !st->background_freeze;
            break;
        case SDLK_BACKSPACE:
            for (int i = 0; i < MAX_WAVES; i++) st->waves[i].alive = false;
            st->hyper_mode = !st->hyper_mode;
            rebuild_window(st);
            setup_bars(st);
            break;
        case SDLK_MINUS:
            st->motion_blur = !st->motion_blur;
            break;
        case SDLK_HASH:
            st->glitch_mode = !st->glitch_mode;
            break;
        case SDLK_F1:
            st->show_fps = !st->show_fps;
            break;
        case SDLK_F2:
            save_config(st);
            break;
        case SDLK_F3:
            load_config(st);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (st->mode == MODE_KEYBOARD) {
                st->mode = MODE_MUSIC;
                st->center_mirror = false;
                st->screen_shake = 0.0f;
            } else {
                st->mode = MODE_KEYBOARD;
            }
            break;
        case SDLK_F4: {
            /* Zyklisch zwischen Zielraten wechseln: 60 -> 120 -> unbegrenzt -> 60 ... */
            if      (st->fps_cap == 60)  st->fps_cap = 120;
            else if (st->fps_cap == 120) st->fps_cap = 0;   /* 0 = unlimitiert */
            else                          st->fps_cap = 60;
            break;
        }
        default:
            break;
    }
}


int main(int argc, char **argv) {
    (void)argc; (void)argv;

    srand((unsigned int)time(NULL));

    AppState st;
    if (!init_state(&st)) {
        shutdown_state(&st);
        return 1;
    }

    Uint64 perf_freq = SDL_GetPerformanceCounter();
    (void)perf_freq;
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 last_counter = SDL_GetPerformanceCounter();

    float fps_smooth = 0.0f;

    while (st.running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                st.running = false;
            } else if (event.type == SDL_KEYDOWN) {
                handle_keydown(&st, event.key.keysym.sym);
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    handle_window_resized(&st, event.window.data1, event.window.data2);
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    handle_mouse_down(&st, event.button.x, event.button.y);
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    handle_mouse_up(&st);
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                handle_mouse_motion(&st, event.motion.x);
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last_counter) / (float)freq;
        last_counter = now;
        if (dt > 0.05f) dt = 0.05f;

        fps_smooth = fps_smooth * 0.9f + (dt > 0.0f ? (1.0f / dt) : 0.0f) * 0.1f;

        if (st.mode == MODE_KEYBOARD) {
            const Uint8 *keystate = SDL_GetKeyboardState(NULL);
            handle_fast_input(&st, keystate);
        }
        update_bars(&st, dt);
        render_frame(&st, fps_smooth);

        if (st.fps_cap > 0) {
            float target_frame_time = 1.0f / (float)st.fps_cap;
            Uint64 after = SDL_GetPerformanceCounter();
            float elapsed = (float)(after - now) / (float)freq;
            if (elapsed < target_frame_time) {
                SDL_Delay((Uint32)((target_frame_time - elapsed) * 1000.0f));
            }
        }
    }

    shutdown_state(&st);
    return 0;
}
