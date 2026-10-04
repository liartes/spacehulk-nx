/* dcr_config.h -- the user's settings, from <game folder>/config.ini (dcr_config.c).
 * Free of libnx so dcr_ilpatch.c keeps building on the host for its test. */
#ifndef DCR_USER_CONFIG_H
#define DCR_USER_CONFIG_H

typedef struct {
  int unlock_all;   /* [game] unlock_all_characters */
  int max_stars;    /* [game] max_character_stars */
  int hide_top_bar; /* [game] hide_top_bar */
  int hop_on_press; /* [controls] hop_on_press */
  int res_w, res_h; /* [display] resolution: 1080 / 720 / auto */
  int boost;        /* [performance] boost_cpu_when_loading */
  int short_wipe;   /* [performance] shorter_transitions */
  int gl_selftest;  /* [debug] gl_selftest */
  int profile;      /* [debug] profile_long_frames */
  int profile_window; /* [debug] profile_continuous */
  int boot_log;     /* [debug] boot_log_on_screen */
  int free_store;   /* [game] free_purchases */
  int log_sounds;   /* [debug] log_sounds */
  int sound_priority; /* [audio] important_sounds_first */
  int quiet_center;   /* [audio] one_theme_switch_sound */
  int mix_48k;        /* [audio] mix_at_48khz */
  int menu_controls;  /* [controls] menu_controls */
  int pretend_online; /* [online] pretend_online */
  int tex_limit;      /* [graphics] texture_resolution: 0 full, 1 half, 2 quarter */
  int minus_capture;  /* [debug] minus_screenshot */
  int threaded;       /* [graphics] threaded_rendering */
  int read_buf_kb;    /* [performance] read_buffer_kb: stdio buffer of read-only files, 0 = newlib's */
  int obb_shared;     /* [performance] obb_one_handle */
  int log_buttons;    /* [debug] log_buttons */
  int voices64;       /* [audio] real_voices_64 */
  int msaa;           /* [graphics] antialiasing: -1 the game's, else 0/2/4 samples */
} DcrConfig;

/* Read config.ini (writing it with the defaults, or adding missing options,
 * first). Early in main(); the defaults hold until then. */
void dcr_config_load(void);
const DcrConfig *dcr_config(void);

#endif
