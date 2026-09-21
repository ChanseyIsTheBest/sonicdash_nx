/* sd_config.h -- config.txt next to the .nro, read at every launch (sd_config.c). */
#ifndef SD_CONFIG_H
#define SD_CONFIG_H
extern int sd_cfg_res;         /* short side in pixels: 720 (default) .. 1080         */
extern int sd_cfg_rotation;    /* 0 none, 1 = 90 clockwise, 2 = 90 counter-clockwise  */
void sd_config_load(void);     /* template on first launch; appends options to old files */
const char *sd_lang(void);     /* the two-letter language the port reports            */
#endif
