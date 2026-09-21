/* sd_saveedit.h -- edit save.txt at boot from save_edit.txt (see sd_saveedit.c). */
#ifndef SD_SAVEEDIT_H
#define SD_SAVEEDIT_H

/* Portable core (tested on a PC): 1 = save changed, 0 = nothing to do, -1 = refused. */
int sd_saveedit_run_paths(const char *save_path, const char *edit_path);

/* Switch entry point: GAME_HOME/save_edit.txt -> GAME_HOME/save.txt (SD_SAVE_EDIT). */
void sd_saveedit_run(void);

#endif /* SD_SAVEEDIT_H */
