/* NaOS settings: key=value file (/SETTINGS.TXT), prompt + console theme. */
#ifndef NAOS_SETTINGS_H
#define NAOS_SETTINGS_H

#include <stdint.h>

void settings_load(void); /* read /SETTINGS.TXT once at shell start */
int settings_save(void);  /* rewrite /SETTINGS.TXT from memory. 0 ok. */
void settings_show(void); /* `settings` command: values + source */
const char *settings_prompt(void);
uint32_t settings_fg(void);
uint32_t settings_bg(void);
int settings_font(void);           /* 1 normal, 2 big */
void settings_set_font(int scale); /* clamp + apply now (no save) */

#endif /* NAOS_SETTINGS_H */
