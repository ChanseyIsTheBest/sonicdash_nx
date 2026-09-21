/* unity_input_hook.h -- patch the game's il2cpp UnityEngine.Input methods to return our
 * Switch touch state. See unity_input_hook.c. */
#ifndef UNITY_INPUT_HOOK_H
#define UNITY_INPUT_HOOK_H

#include <stdint.h>

/* Install the hooks. Call once after libil2cpp is loaded+finalized (il2cpp load_virtbase). */
void nx_install_input_hooks(uintptr_t il2cpp_base);

/* Route UnityEngine.PlayerPrefs (the game's save) through our persistent prefs.kv store, since
 * Unity's native PlayerPrefs never writes to disk on Switch. string_new = il2cpp_string_new. */
void nx_install_playerprefs_hooks(uintptr_t il2cpp_base, void *string_new);

/* Max simultaneous fingers we report to Unity (Switch panel tracks up to 16; 10 is plenty). */
#define NX_MAX_TOUCH 10

/* One finger, in Unity screen space (bottom-left origin, game px). `id` is the HID finger id:
 * it must stay stable while the finger is down so phases (Began/Moved/Ended) track correctly. */
typedef struct { int id; float x, y; } NxTouchIn;

/* Push ALL currently-down fingers each frame (n may be 0). Phases are derived by matching
 * finger ids against last frame. This is the multi-touch entry point. */
void nx_input_hook_update_multi(const NxTouchIn *in, int n);

/* Single-touch convenience wrapper (stick-cursor path). Equivalent to update_multi with
 * one finger (id 0) when active, or zero fingers when not. */
void nx_input_hook_update(int active, float ux, float uy);

/* The game's UI resolution (UnityEngine.Screen), 0 if not yet available. */
int nx_input_game_screen(int *w, int *h);

/* Game tweaks (config.h: SD_FULL_RES_RENDER, SD_ANALYTICS_OFF). */
void nx_install_tweak_hooks(uintptr_t il2cpp_base);

#endif /* UNITY_INPUT_HOOK_H */
