/* sd_tate.h -- from bloonspop_nx bp_tate.h; rotation is a runtime setting here. */
/* clayjam_tate.h -- portrait ("TATE") presentation for Clay Jam Classic.
 *
 * Clay Jam is portrait-only. The Switch will not give us a portrait window, so
 * the engine renders into a portrait FBO and we rotate it onto the landscape
 * panel ourselves. See clayjam_tate.c for why the window cannot simply be
 * resized.
 *
 * The DISPLAY rotation and the INVERSE TOUCH mapping both live in this one
 * file, deliberately. papersplease_nx had them in separate translation units
 * driven by the same config value, and when its UV table was written backwards
 * the display rotated one way while the pointer maths assumed the other --
 * taps landed on the wrong axis and it read as two unrelated bugs. Keeping the
 * forward and inverse transforms adjacent means a mistake in one is visible
 * against the other. That matters more here than it did there: Clay Jam is a
 * continuous drag that gouges channels in the clay, so a pointer that is
 * rotated wrong is not a mis-tap, it is an unplayable game.
 */
#ifndef SD_TATE_H
#define SD_TATE_H

#include <GLES3/gl3.h>

/* rot: 1 = 90 CW, 2 = 90 CCW. render_* is the portrait size the engine believes
 * it has; window_* is the real landscape swapchain. Returns 1 on success, 0 if
 * unavailable -- in which case every entry point below is inert and the port
 * falls back to rendering straight to the window. */
int    sd_tate_init(int render_w, int render_h, int window_w, int window_h, int rot);

int    sd_tate_active(void);      /* non-zero once we own the default FB   */
GLuint sd_tate_fbo(void);         /* substitute when the engine binds 0    */
void   sd_tate_present(void);     /* rotated blit; call before eglSwapBuffers */
void   sd_tate_shutdown(void);

/* Inverse of the display rotation: panel/touch coordinates -> the coordinate
 * space the engine believes it is running in. `panel_w/panel_h` is the space
 * the touchscreen reports in (1280x720 on Switch, in BOTH modes -- the touch
 * panel does not rescale when docked). Safe to call before init; without an
 * active rotation it degrades to a plain scale. */
void   sd_tate_map_pointer(float px, float py,
                           float panel_w, float panel_h,
                           float *out_x, float *out_y);

/* --- glue (runtime rotation from config.txt) ------------------------------ */
int  sd_tate_enabled(void);        /* config.txt rotation != 0                */
void sd_tate_bind_overlay(void);   /* bind the portrait FBO for nxp_draw()    */
void sd_tate_swap_hook(void);      /* lazy init + rotated present; before swap */
void sd_tate_map_stick(float dx, float dy, float *rx, float *ry);
void sd_gl_BindFramebuffer_tate(GLenum target, GLuint fb);   /* GL import table */
#endif /* SD_TATE_H */
