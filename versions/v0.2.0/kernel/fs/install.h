/* NaOS system installer: live session -> bootable disk. Own code. */
#ifndef NAOS_INSTALL_H
#define NAOS_INSTALL_H

/* List probed disks + the live session. 0 ok. */
int drives_show(void);
/* Copy the running system onto drive <id> ("ahci0", "nvme", "ata").
 * erase must be 1 (the word `erase`): formats the target. Refuses the
 * boot volume. 0 ok. */
int install_run(const char *id, int erase);

#endif /* NAOS_INSTALL_H */
