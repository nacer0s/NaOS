/* NaOS settings: key=value file (/SETTINGS.TXT), prompt + console theme. */
#ifndef NAOS_SETTINGS_H
#define NAOS_SETTINGS_H

#include <stdint.h>

void settings_load(void); /* read /SETTINGS.TXT once at shell start */
void settings_show(void); /* `settings` command: values + source */
const char *settings_prompt(void);
uint32_t settings_fg(void);
uint32_t settings_bg(void);

#endif /* NAOS_SETTINGS_H */
