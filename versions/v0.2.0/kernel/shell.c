/* NaOS shell. Reads PS/2 ASCII, echoes, dispatches builtins. */
#include "shell.h"
#include "version.h"
#include "drv/fb.h"
#include "drv/pit.h"
#include "drv/ps2_kbd.h"
#include "drv/rtc.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "fs/fat32.h"
#include "fs/nafs.h"
#include "fs/install.h"
#include "drv/rtc.h"
#include "drv/acpi.h"
#include "drv/speaker.h"
#include "drv/pci.h"
#include "drv/blk.h"
#include "drv/usb_kbd.h"
#include "drv/nvme.h"
#include "net/net.h"
#include "net/dhcp.h"
#include "net/tcp.h"
#include "net/dns.h"
#include "net/update.h"
#include "net/tls.h"
#include "settings.h"
#include "task.h"
#include "user/progs.h"
#include "../common/cstring.h"
#include "../common/serial.h"

#define LINE_MAX 256
#define CWD_MAX 128

static size_t read_line(char *line); /* defined below shell_run helpers */

/* Current folder, always absolute ("/" = root). Plain text; the FAT
 * lookup is case-insensitive so typed case is preserved for display. */
static char g_cwd[CWD_MAX] = "/";

/* Join the current folder with a user path into out (absolute result).
 * Leading / or \ means absolute already. */
static void mkpath(const char *arg, char *out, size_t outsz) {
  size_t n = 0;
  if (arg[0] != '/' && arg[0] != '\\') {
    size_t c = 0;
    while (g_cwd[c] && n + 1 < outsz) { out[n++] = g_cwd[c++]; }
    if (!(n == 1 && out[0] == '/')) {
      if (n + 1 < outsz) out[n++] = '/';
    }
  }
  size_t i = 0;
  while (arg[i] && n + 1 < outsz) { out[n++] = arg[i++]; }
  out[n] = '\0';
}

/* Split args into words in place (spaces/tabs). Returns word count. */
static int split_words(char *a, char *w[], int max) {
  int n = 0;
  while (*a && n < max) {
    while (*a == ' ' || *a == '\t') a++;
    if (!*a) break;
    w[n++] = a;
    while (*a && *a != ' ' && *a != '\t') a++;
    if (*a) {
      *a = '\0';
      a++;
    }
  }
  return n;
}

/* Match a whole command word: returns pointer to args (skipping spaces),
 * or 0 if `line` is not exactly `cmd` or `cmd ...`. "listen" is not "list". */
static const char *after(const char *line, const char *cmd) {
  while (*cmd) {
    if (*line != *cmd) return 0;
    line++; cmd++;
  }
  if (*line == '\0') return line;
  if (*line == ' ') {
    while (*line == ' ') line++;
    return line;
  }
  return 0;
}

#define EDIT_MAX 8192

/* Append-only editor: shows file, reads lines, .save keeps, .quit drops. */
static void cmd_edit(const char *path) {
  static char buf[EDIT_MAX];
  uint32_t size = 0;
  int r = fat_read(path, buf, sizeof(buf) - 1, &size);
  size_t n;
  if (r < 0) {
    kprintf("(new file)\n");
    n = 0;
  } else {
    n = (size_t)r;
    buf[n] = '\0';
    kprintf("--- %s (%u bytes) ---\n%s", path, (unsigned)size, buf);
    if (n > 0 && buf[n - 1] != '\n') kprintf("\n");
    kprintf("--- end ---\n");
  }
  static char line[LINE_MAX];
  for (;;) {
    kprintf("> ");
    size_t m = read_line(line);
    if (naos_strcmp(line, ".save") == 0) {
      int w = fat_write(path, buf, (uint32_t)n);
      if (w == 0) kprintf("saved %s (%u bytes)\n", path, (unsigned)n);
      else kprintf("save failed (%d)\n", w);
      return;
    }
    if (naos_strcmp(line, ".quit") == 0) {
      kprintf("dropped\n");
      return;
    }
    if (n + m + 2 >= EDIT_MAX) {
      kprintf("full (8192 bytes max)\n");
      continue;
    }
    for (size_t i = 0; i < m; i++) buf[n++] = line[i];
    buf[n++] = '\n';
  }
}

static void cmd_mem(void) {
  uint64_t total = pmm_total_pages();
  uint64_t free = pmm_free_pages();
  kprintf("mem: total=%u pages (%u KB) free=%u pages (%u KB) heap_used=%u bytes\n",
          (unsigned)total, (unsigned)(total * 4u),
          (unsigned)free, (unsigned)(free * 4u),
          (unsigned)kmalloc_used_bytes());
}

static void cmd_heap(void) {
  /* Alloc/free workout: catches allocator corruption live. */
  char *a = (char *)kmalloc(64);
  char *b = (char *)kmalloc(128);
  char *c = (char *)kmalloc(32);
  if (!a || !b || !c) { kprintf("heap: OOM\n"); return; }
  naos_memset(a, 0xAA, 64);
  naos_memset(b, 0xBB, 128);
  naos_memset(c, 0xCC, 32);
  kprintf("heap: a=%p b=%p c=%p used=%u\n", a, b, c,
          (unsigned)kmalloc_used_bytes());
  kfree(b);
  kprintf("heap: freed b, used=%u\n", (unsigned)kmalloc_used_bytes());
  char *d = (char *)kmalloc(100); /* should reuse b's slot */
  kprintf("heap: d=%p (reuse %s) used=%u\n", d,
          (d == b) ? "OK" : "elsewhere", (unsigned)kmalloc_used_bytes());
  int ok = 1;
  for (int i = 0; i < 64; i++) if (a[i] != (char)0xAA) ok = 0;
  for (int i = 0; i < 32; i++) if (c[i] != (char)0xCC) ok = 0;
  kprintf("heap: neighbors intact %s\n", ok ? "OK" : "CORRUPT");
  kfree(a); kfree(c); kfree(d);
  kprintf("heap: all freed, used=%u\n", (unsigned)kmalloc_used_bytes());
}

#define MAX_ARGS 17 /* up to 16 sources + destination */

static char g_argbuf[MAX_ARGS][96];
static const char *g_argv[MAX_ARGS];

/* Split args on spaces into g_argv; returns count. */
static int split_args(const char *a) {
  int n = 0;
  while (*a && n < MAX_ARGS) {
    while (*a == ' ') a++;
    if (*a == '\0') break;
    size_t i = 0;
    while (*a && *a != ' ' && i + 1 < sizeof(g_argbuf[0])) {
      g_argbuf[n][i++] = *a++;
    }
    /* Skip overlong tail of this arg. */
    while (*a && *a != ' ') a++;
    g_argbuf[n][i] = '\0';
    if (i > 0) {
      g_argv[n] = g_argbuf[n];
      n++;
    }
  }
  return n;
}

/* Last piece of a path (after / or \). */
static const char *base_name(const char *p) {
  const char *b = p;
  for (const char *q = p; *q; q++)
    if (*q == '/' || *q == '\\') b = q + 1;
  return b;
}

static void dispatch(char *line) {
  const char *a;
  static char full[256]; /* joined absolute path for file commands */
  if (line[0] == '\0') return;
  /* Output redirect: <command> > <file>. Split at the first '>'. */
  {
    char *gt = 0;
    for (char *p = line; *p; p++) {
      if (*p == '>') { gt = p; break; }
    }
    if (gt) {
      static char cap[8192];
      char *rhs;
      size_t used;
      int full_flag, w;
      *gt = '\0';
      rhs = gt + 1;
      while (*rhs == ' ') rhs++;
      {
        char *end = rhs;
        while (*end) end++;
        while (end > rhs && end[-1] == ' ') end--;
        *end = '\0';
      }
      if (after(line, "edit-file")) {
        kprintf("redirect: edit-file is interactive, cannot redirect\n");
        return;
      }
      {
        int more = 0;
        for (char *p = rhs; *p; p++) {
          if (*p == '>') { more = 1; break; }
        }
        if (more) {
          kprintf("redirect: only one > allowed\n");
          return;
        }
      }
      if (*rhs == '\0') {
        kprintf("usage: <command> > <file>\n");
        return;
      }
      mkpath(rhs, full, sizeof(full));
      /* Copy aside: the nested dispatch reuses `full`. */
      {
        static char savepath[256];
        size_t i = 0;
        while (full[i] && i + 1 < sizeof(savepath)) {
          savepath[i] = full[i];
          i++;
        }
        savepath[i] = '\0';
        kprintf_begin_capture(cap, sizeof(cap));
        dispatch(line);
        used = kprintf_end_capture();
        full_flag = kprintf_capture_full();
        w = fat_write(savepath, cap, (uint32_t)used);
        if (w == 0)
          kprintf("saved %u bytes to %s%s\n", (unsigned)used, savepath,
                  full_flag ? " (truncated at 8192)" : "");
        else
          kprintf("save failed (%d)\n", w);
      }
      return;
    }
  }
  if (after(line, "help")) {
    kprintf("commands:\n");
    kprintf("  help               this list\n");
    kprintf("  show-version       show NaOS version\n");
    kprintf("  about-system       developer, links, licence\n");
    kprintf("  clear-screen       clear the screen\n");
    kprintf("  print-text <text>  show text on screen\n");
    kprintf("  show-memory        show memory info\n");
    kprintf("  run-tests          check the memory allocator\n");
    kprintf("  force-crash        force a page fault (reboot after)\n");
    kprintf("  show-files [dir]   list files\n");
    kprintf("  open-folder <dir>  go to a folder (open-folder .. goes back, open-folder / goes home)\n");
    kprintf("  read-file <file>   show a file\n");
    kprintf("  make-folder <name> make a folder\n");
    kprintf("  make-file <name>   make an empty file\n");
    kprintf("  copy-file <a...> <to>   copy files or folders into a folder\n");
    kprintf("  move-file <a...> <to>   move files or folders into a folder\n");
    kprintf("  rename-file <old> <new> rename a file or folder\n");
    kprintf("  edit-file <file>   write a file (.save to keep, .quit to drop)\n");
    kprintf("  delete-file <name> delete a file or empty folder\n");
    kprintf("  run-program <prog> start a program (counter, ticker, peek, or file)\n");
    kprintf("  show-tasks         show programs\n");
    kprintf("  stop-task <id>     stop a program\n");
    kprintf("  show-time          show the time\n");
    kprintf("  show-date          show the date\n");
    kprintf("  show-uptime        show time since boot\n");
    kprintf("  play-beep          play a sound\n");
    kprintf("  show-pci           list PCI devices (disk driver: %s)\n",
            blk_name());
    kprintf("  find-keyboard      look for a USB keyboard now\n");
    kprintf("  show-nvme          NVMe info + write/read selftest\n");
    kprintf("  show-drives        list disks + the live session\n");
    kprintf("  install-system <drive> erase\n");
    kprintf("                     copy the live system onto a disk\n");
    kprintf("  file-system mount <img>   mount a NaOS-FS image file\n");
    kprintf("  file-system list [/dir]   list the mounted NaOS-FS\n");
    kprintf("  file-system read </file>  print a NaOS-FS text file\n");
    kprintf("  file-system write <s> <d> copy a FAT file into the image\n");
    kprintf("  file-system delete </file> delete a file from the image\n");
    kprintf("  show-network       show network status\n");
    kprintf("  ping-address <ip>  ping an IPv4 address\n");
    kprintf("  get-address        get IP via DHCP (proves UDP)\n");
    kprintf("  find-address <name> resolve a hostname\n");
    kprintf("  system-update check <host> <port> </manifest>\n");
    kprintf("                     compare versions, list files\n");
    kprintf("  system-update get <host> <port> </remote> </local> [sha]\n");
    kprintf("                     download a file, verify its hash\n");
    kprintf("  system-update check-tls <host> <port> </manifest>\n");
    kprintf("                     same, over own TLS 1.2\n");
    kprintf("  system-update get-tls <host> <port> </remote> </local> [sha]\n");
    kprintf("                     same, over own TLS 1.2\n");
    kprintf("  get-page <ip> <port> </path>\n");
    kprintf("                     HTTP/1.0 GET over TCP, print body\n");
    kprintf("  get-page-tls <host> <port> </path>\n");
    kprintf("                     HTTPS GET (own TLS 1.2), print body\n");
    kprintf("  serve-file <port> </file>\n");
    kprintf("                     serve one file over HTTP, then done\n");
    kprintf("  show-settings      show prompt and colors (/SETTINGS.TXT)\n");
    kprintf("  set-font [small|normal|big|3|4]\n");
    kprintf("                     console size, saved\n");
    kprintf("  PgUp/PgDn scroll the screen back and forth\n");
    kprintf("  Ctrl+C             cancel the current line\n");
    kprintf("  reboot-system      restart the computer\n");
    kprintf("  shut-down          stop the computer\n");
    kprintf("  <cmd> > <file>     save command output to a file\n");
  } else if (after(line, "show-version")) {
    kprintf("NaOS v%s\n", NAOS_VERSION);
    kprintf("build %s (x86_64 UEFI, from scratch)\n", NAOS_BUILD);
  } else if (after(line, "about-system")) {
    kprintf("NaOS v%s (%s) - usable x86_64 UEFI OS, from scratch in C\n",
            NAOS_VERSION, NAOS_BUILD);
    kprintf("Developer: nacer0s - github.com/nacer0s - nacer0s.com\n");
    kprintf("Licence: source-available, non-commercial (see LICENCE.md)\n");
  } else if (after(line, "clear-screen")) {
    fb_clear(settings_bg());
  } else if (after(line, "show-settings")) {
    settings_show();
  } else if ((a = after(line, "set-font"))) {
    if (*a == '\0') {
      int f = settings_font();
      kprintf("font is %s\n", f == 0 ? "small" : f == 1 ? "normal" : f == 2 ? "big" : f == 3 ? "3" : "4");
    } else {
      int f = -1;
      if (naos_strcmp(a, "small") == 0) f = 0;
      else if (naos_strcmp(a, "normal") == 0 || naos_strcmp(a, "1") == 0)
        f = 1;
      else if (naos_strcmp(a, "big") == 0 || naos_strcmp(a, "2") == 0)
        f = 2;
      else if (naos_strcmp(a, "3") == 0) f = 3;
      else if (naos_strcmp(a, "4") == 0) f = 4;
      if (f < 0) {
        kprintf("usage: set-font [small|normal|big|3|4]\n");
      } else {
        settings_set_font(f);
        if (settings_save() == 0)
          kprintf("font is %s (saved)\n", a);
        else
          kprintf("font changed (save failed)\n");
      }
    }
  } else if ((a = after(line, "print-text"))) {
    kprintf("%s\n", a);
  } else if (after(line, "show-memory")) {
    cmd_mem();
  } else if (after(line, "run-tests")) {
    cmd_heap();
  } else if (after(line, "force-crash")) {
    kprintf("crash: reading unmapped 0x50000000 (expect PAGE FAULT)...\n");
    volatile uint8_t v = *(volatile uint8_t *)(uintptr_t)0x50000000ULL;
    kprintf("crash: survived?! v=%u\n", (unsigned)v);
  } else if ((a = after(line, "show-files"))) {
    mkpath(a, full, sizeof(full));
    fat_ls(full);
  } else if ((a = after(line, "open-folder"))) {
    const char *orig = a;
    if (*a == '\0' || naos_strcmp(a, "/") == 0) {
      g_cwd[0] = '/';
      g_cwd[1] = '\0';
    } else {
      /* Normalize against the current folder, one piece at a time. */
      static char tmp[CWD_MAX];
      size_t n = 0;
      if (a[0] != '/' && a[0] != '\\') {
        size_t c = 0;
        while (g_cwd[c] && n + 1 < sizeof(tmp)) { tmp[n++] = g_cwd[c++]; }
      } else {
        while (*a == '/' || *a == '\\') a++;
      }
      tmp[n] = '\0';
      char piece[32];
      int bad = 0;
      while (*a && !bad) {
        while (*a == '/' || *a == '\\') a++;
        if (*a == '\0') break;
        size_t m = 0;
        while (*a && *a != '/' && *a != '\\' && m + 1 < sizeof(piece))
          piece[m++] = *a++;
        piece[m] = '\0';
        if (naos_strcmp(piece, ".") == 0) continue;
        if (naos_strcmp(piece, "..") == 0) {
          /* Drop the last piece of tmp (stay home at "/"). */
          while (n > 1 && tmp[n - 1] != '/') n--;
          if (n > 1) n--; /* drop the slash too */
          tmp[n] = '\0';
          continue;
        }
        if (!(n == 1 && tmp[0] == '/')) {
          if (n + 1 >= sizeof(tmp)) { bad = 1; break; }
          tmp[n++] = '/';
        }
        size_t k = 0;
        while (piece[k]) {
          if (n + 1 >= sizeof(tmp)) { bad = 1; break; }
          tmp[n++] = piece[k++];
        }
        tmp[n] = '\0';
      }
      if (bad || !fat_is_dir(tmp[0] ? tmp : "/")) {
        kprintf("open-folder: no such folder: %s\n", orig);
      } else {
        size_t c = 0;
        while (tmp[c] && c + 1 < sizeof(g_cwd)) {
          g_cwd[c] = tmp[c];
          c++;
        }
        g_cwd[c] = '\0';
        if (g_cwd[0] == '\0') {
          g_cwd[0] = '/';
          g_cwd[1] = '\0';
        }
      }
    }
  } else if ((a = after(line, "read-file"))) {
    if (*a == '\0') {
      kprintf("usage: read-file <file>\n");
    } else {
      static char buf[4096];
      uint32_t size = 0;
      mkpath(a, full, sizeof(full));
      int r = fat_read(full, buf, sizeof(buf) - 1, &size);
      if (r < 0) {
        kprintf("read-file: cannot read %s (err %d)\n", full, r);
      } else {
        buf[r] = '\0';
        kprintf("--- %s (%u bytes%s) ---\n", full, (unsigned)size,
                (size > (uint32_t)r) ? ", truncated" : "");
        kprintf("%s", buf);
        if (r > 0 && buf[r - 1] != '\n') kprintf("\n");
      }
    }
  } else if ((a = after(line, "edit-file"))) {
    if (*a == '\0') {
      kprintf("usage: edit-file <file>\n");
    } else {
      mkpath(a, full, sizeof(full));
      cmd_edit(full);
    }
  } else if ((a = after(line, "make-folder"))) {
    if (*a == '\0') {
      kprintf("usage: make-folder <name>\n");
    } else {
      mkpath(a, full, sizeof(full));
      int r = fat_mkdir(full);
      if (r == 0) kprintf("made %s\n", full);
      else kprintf("make-folder: failed (%d)\n", r);
    }
  } else if ((a = after(line, "make-file"))) {
    if (*a == '\0') {
      kprintf("usage: make-file <name>\n");
    } else {
      mkpath(a, full, sizeof(full));
      int r = fat_write(full, "", 0);
      if (r == 0) kprintf("made %s\n", full);
      else kprintf("make-file: failed (%d)\n", r);
    }
  } else if (after(line, "make")) {
    kprintf("usage: make-folder <name> / make-file <name>\n");
  } else if ((a = after(line, "delete-file"))) {
    if (*a == '\0') {
      kprintf("usage: delete-file <name>\n");
    } else {
      mkpath(a, full, sizeof(full));
      int r = fat_delete(full);
      if (r == 0) kprintf("deleted %s\n", full);
      else kprintf("delete-file: failed (%d)\n", r);
    }
  } else if ((a = after(line, "copy-file")) || (a = after(line, "move-file"))) {
    int is_move = (line[0] == 'm'); /* "move" vs "copy" */
    int n = split_args(a);
    static char dstf[256], srcf[256];
    if (n < 2) {
      kprintf(is_move ? "usage: move-file <a...> <to folder>\n"
                      : "usage: copy-file <a...> <to folder>\n");
    } else {
      /* Destination: last word. Bare name = same folder. */
      mkpath(g_argv[n - 1], dstf, sizeof(dstf));
      int dst_is_dir = fat_is_dir(dstf);
      if (n > 2 && !dst_is_dir) {
        kprintf(is_move ? "move: %s is not a folder\n"
                        : "copy: %s is not a folder\n",
                dstf);
      } else {
        for (int i = 0; i + 1 < n; i++) {
          mkpath(g_argv[i], srcf, sizeof(srcf));
          if (n == 2 && !dst_is_dir) {
            /* Two words, target missing: new name in place. */
            if (is_move) {
              int r = fat_rename(srcf, dstf);
              if (r == 0) kprintf("moved %s\n", srcf);
              else kprintf("move: failed (%d)\n", r);
            } else {
              int r = fat_copy(srcf, dstf);
              if (r >= 0) kprintf("copied %u bytes\n", (unsigned)r);
              else kprintf("copy: failed (%d)\n", r);
            }
          } else {
            /* Into a folder, keeping the name. */
            static char target[256];
            size_t m = 0;
            while (dstf[m] && m + 1 < sizeof(target)) {
              target[m] = dstf[m];
              m++;
            }
            if (!(m == 1 && target[0] == '/')) {
              if (m + 1 < sizeof(target)) target[m++] = '/';
            }
            const char *b = base_name(srcf);
            size_t k = 0;
            while (b[k] && m + 1 < sizeof(target)) target[m++] = b[k++];
            target[m] = '\0';
            if (is_move) {
              int r = fat_rename(srcf, target);
              if (r == 0) kprintf("moved %s\n", srcf);
              else kprintf("move: failed (%d)\n", r);
            } else {
              int r = fat_copy(srcf, target);
              if (r >= 0) kprintf("copied %u bytes\n", (unsigned)r);
              else kprintf("copy: failed (%d)\n", r);
            }
          }
        }
      }
    }
  } else if ((a = after(line, "rename-file"))) {
    int n = split_args(a);
    if (n != 2) {
      kprintf("usage: rename-file <old> <new name>\n");
    } else {
      static char oldf[256];
      mkpath(g_argv[0], oldf, sizeof(oldf));
      /* New value must be a plain name (no folders): rename stays put. */
      int slashes = 0;
      for (const char *p = g_argv[1]; *p; p++)
        if (*p == '/' || *p == '\\') slashes = 1;
      if (slashes) {
        kprintf("rename-file: new name only, use move-file to change folders\n");
      } else {
        /* Parent folder of the old path + new leaf. */
        static char newf[256];
        size_t L = 0;
        while (oldf[L]) L++;
        size_t cut = L;
        while (cut > 0 && oldf[cut - 1] != '/' && oldf[cut - 1] != '\\') cut--;
        size_t m = 0;
        for (size_t i = 0; i < cut && m + 1 < sizeof(newf); i++)
          newf[m++] = oldf[i];
        size_t k = 0;
        while (g_argv[1][k] && m + 1 < sizeof(newf)) newf[m++] = g_argv[1][k++];
        newf[m] = '\0';
        int r = fat_rename(oldf, newf);
        if (r == 0) kprintf("renamed %s\n", oldf);
        else kprintf("rename-file: failed (%d)\n", r);
      }
    }
  } else if ((a = after(line, "run-program"))) {
    if (*a == '\0') {
      kprintf("usage: run-program <program> (counter, ticker, peek, or file)\n");
    } else if (naos_strcmp(a, "counter") == 0) {
      int id = task_spawn("counter", prog_counter);
      if (id >= 0) kprintf("started counter as program %u\n", (unsigned)id);
      else kprintf("run: failed (%d)\n", id);
    } else if (naos_strcmp(a, "ticker") == 0) {
      int id = task_spawn("ticker", prog_ticker);
      if (id >= 0) kprintf("started ticker as program %u\n", (unsigned)id);
      else kprintf("run: failed (%d)\n", id);
    } else if (naos_strcmp(a, "peek") == 0) {
      int id = task_spawn("peek", prog_peek);
      if (id >= 0) kprintf("started peek as program %u\n", (unsigned)id);
      else kprintf("run: failed (%d)\n", id);
    } else {
      mkpath(a, full, sizeof(full));
      int id = task_spawn_elf(full, full);
      if (id >= 0) kprintf("started %s as program %u\n", full, (unsigned)id);
      else kprintf("run-program: no program and no file %s (%d)\n", full, id);
    }
  } else if (after(line, "show-tasks")) {
    task_list();
  } else if (after(line, "show-time")) {
    rtc_time_t t;
    if (rtc_read(&t)) {
      char b[16];
      rtc_format_time(&t, b, sizeof(b));
      kprintf("%s\n", b);
    } else {
      kprintf("time: no clock\n");
    }
  } else if (after(line, "show-date")) {
    rtc_time_t t;
    if (rtc_read(&t)) {
      char b[16];
      rtc_format_date(&t, b, sizeof(b));
      kprintf("%s\n", b);
    } else {
      kprintf("date: no clock\n");
    }
  } else if (after(line, "show-uptime")) {
    uint64_t s = pit_ticks() / 100;
    kprintf("up %u:%u:%u\n", (unsigned)(s / 3600),
            (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  } else if (after(line, "play-beep")) {
    speaker_beep(523, 120);
    speaker_beep(659, 120);
    speaker_beep(784, 200);
  } else if (after(line, "show-pci")) {
    kprintf("disk driver: %s\n", blk_name());
    pci_list();
  } else if (after(line, "find-keyboard")) {
    usb_kbd_rescan();
  } else if ((a = after(line, "show-nvme"))) {
    (void)a;
    nvme_info();
    kprintf("nvme: selftest %s\n", nvme_selftest() == 0 ? "OK" : "FAIL");
  } else if (after(line, "show-drives")) {
    drives_show();
  } else if ((a = after(line, "install-system"))) {
    int n = split_args(a);
    if (n == 0) install_run(0, 0);
    else if (n == 1) install_run(g_argv[0], 0);
    else install_run(g_argv[0], naos_strcmp(g_argv[1], "erase") == 0);
  } else if ((a = after(line, "file-system"))) {
    char *w[4];
    int n = split_words((char *)a, w, 4);
    if (n >= 2 && naos_strcmp(w[0], "mount") == 0) nafs_mount(w[1]);
    else if (n >= 1 && naos_strcmp(w[0], "list") == 0)
      nafs_list(n >= 2 ? w[1] : "/");
    else if (n >= 2 && naos_strcmp(w[0], "read") == 0) nafs_read(w[1]);
    else if (n >= 3 && naos_strcmp(w[0], "write") == 0)
      nafs_write(w[1], w[2]);
    else if (n >= 2 && naos_strcmp(w[0], "delete") == 0) nafs_delete(w[1]);
    else {
      kprintf("usage: file-system mount <fat-path-to-image>\n");
      kprintf("       file-system list [/dir]\n");
      kprintf("       file-system read </file>\n");
      kprintf("       file-system write <fat-src> </nafs-dst>\n");
      kprintf("       file-system delete </file>\n");
    }
  } else if (after(line, "show-network")) {
    net_status();
  } else if (after(line, "get-address")) {
    if (net_dhcp() == 0) net_status();
  } else if ((a = after(line, "find-address"))) {
    if (*a == '\0') {
      kprintf("usage: find-address <name>\n");
    } else {
      uint32_t ip;
      char ips[20];
      if (net_parse_ip(a, &ip) == 0 || dns_resolve(a, &ip) == 0) {
        net_ip_str(ip, ips);
        kprintf("dns: %s is %s\n", a, ips);
      } else {
        kprintf("dns: no answer for '%s'\n", a);
      }
    }
  } else if ((a = after(line, "system-update"))) {
    char *w[6];
    int n = split_words((char *)a, w, 6);
    unsigned port = 0;
    if (n >= 4 && naos_strcmp(w[0], "check") == 0) {
      for (const char *p = w[2]; *p >= '0' && *p <= '9'; p++)
        port = port * 10 + (unsigned)(*p - '0');
      if (port > 0 && port < 65536 && w[3][0] == '/')
        update_check(w[1], (uint16_t)port, w[3]);
      else
        kprintf("usage: system-update check <host> <port> </manifest>\n");
    } else if (n >= 5 && naos_strcmp(w[0], "get") == 0) {
      for (const char *p = w[2]; *p >= '0' && *p <= '9'; p++)
        port = port * 10 + (unsigned)(*p - '0');
      if (port > 0 && port < 65536 && w[3][0] == '/' && w[4][0] != '\0')
        update_get(w[1], (uint16_t)port, w[3], w[4], n >= 6 ? w[5] : "");
      else
        kprintf("usage: system-update get <host> <port> </remote> </local> [sha]\n");
    } else if (n >= 4 && naos_strcmp(w[0], "check-tls") == 0) {
      for (const char *p = w[2]; *p >= '0' && *p <= '9'; p++)
        port = port * 10 + (unsigned)(*p - '0');
      if (port > 0 && port < 65536 && w[3][0] == '/')
        update_check_tls(w[1], (uint16_t)port, w[3]);
      else
        kprintf("usage: system-update check-tls <host> <port> </manifest>\n");
    } else if (n >= 5 && naos_strcmp(w[0], "get-tls") == 0) {
      for (const char *p = w[2]; *p >= '0' && *p <= '9'; p++)
        port = port * 10 + (unsigned)(*p - '0');
      if (port > 0 && port < 65536 && w[3][0] == '/' && w[4][0] != '\0')
        update_get_tls(w[1], (uint16_t)port, w[3], w[4], n >= 6 ? w[5] : "");
      else
        kprintf("usage: system-update get-tls <host> <port> </remote> </local> [sha]\n");
    } else {
      kprintf("usage: system-update check <host> <port> </manifest>\n");
      kprintf("       update get <host> <port> </remote> </local> [sha]\n");
      kprintf("       update check-tls <host> <port> </manifest>\n");
      kprintf("       update get-tls <host> <port> </remote> </local> [sha]\n");
    }
  } else if ((a = after(line, "get-page"))) {
    uint32_t ip = 0;
    unsigned port = 0;
    const char *p = a;
    int ok = 0;
    while (*p && *p != ' ' && *p != '\t') p++; /* skip ip word */
    {
      char saved = *p;
      /* Temporarily terminate the IP word. `line` is our own buffer. */
      char *w = (char *)a;
      w[p - a] = '\0';
      ok = (net_parse_ip(a, &ip) == 0 && saved != '\0');
      if (ok) {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        while (*p >= '0' && *p <= '9' && port < 100000) {
          port = port * 10 + (unsigned)(*p - '0');
          p++;
        }
        while (*p == ' ' || *p == '\t') p++;
        ok = (port > 0 && port < 65536 && *p == '/');
        if (ok) net_fetch(ip, (uint16_t)port, p);
      }
      if (!ok)
        kprintf("usage: get-page <a.b.c.d> <port> </path>\n");
    }
  } else if ((a = after(line, "get-page-tls"))) {
    char *w[4];
    int n = split_words((char *)a, w, 4);
    unsigned port = 0;
    if (n == 3) {
      for (const char *p = w[1]; *p >= '0' && *p <= '9'; p++)
        port = port * 10 + (unsigned)(*p - '0');
    }
    if (n == 3 && port > 0 && port < 65536 && w[2][0] == '/') {
      uint32_t ip = 0;
      uint8_t ipb[4];
      const char *host = 0;
      if (net_parse_ip(w[0], &ip) == 0) {
        ipb[0] = (uint8_t)(ip >> 24);
        ipb[1] = (uint8_t)(ip >> 16);
        ipb[2] = (uint8_t)(ip >> 8);
        ipb[3] = (uint8_t)ip;
        if (tls_fetch_print(ip, (uint16_t)port, 0, ipb, w[2]) != 0)
          kprintf("get-page-tls: failed\n");
      } else if (dns_resolve(w[0], &ip) == 0) {
        host = w[0];
        if (tls_fetch_print(ip, (uint16_t)port, host, 0, w[2]) != 0)
          kprintf("get-page-tls: failed\n");
      } else {
        kprintf("get-page-tls: cannot resolve '%s'\n", w[0]);
      }
    } else {
      kprintf("usage: get-page-tls <host-or-ip> <port> </path>\n");
    }
  } else if ((a = after(line, "serve-file"))) {
    char *w[4];
    int n = split_words((char *)a, w, 4);
    unsigned port = 0;
    if (n == 2) {
      for (const char *p = w[0]; *p >= '0' && *p <= '9'; p++)
        port = port * 10 + (unsigned)(*p - '0');
    }
    if (n == 2 && port > 0 && port < 65536 && w[1][0] == '/')
      net_serve((uint16_t)port, w[1]);
    else
      kprintf("usage: serve-file <port> </file>\n");
  } else if ((a = after(line, "ping-address"))) {
    uint32_t ip;
    if (*a == '\0' || net_parse_ip(a, &ip) != 0)
      kprintf("usage: ping-address <a.b.c.d>\n");
    else
      net_ping(ip);
  } else if (after(line, "reboot-system")) {
    kprintf("restarting...\n");
    cpu_reboot();
    kprintf("reboot: failed\n");
  } else if ((a = after(line, "stop-task"))) {
    if (*a == '\0') {
      kprintf("usage: stop-task <id>\n");
    } else {
      int id = 0;
      int digits = 0;
      for (const char *p = a; *p >= '0' && *p <= '9' && digits < 3; p++) {
        id = id * 10 + (*p - '0');
        digits++;
      }
      int r = task_kill(id);
      if (r == 0) kprintf("stopped program %u\n", (unsigned)id);
      else kprintf("kill: bad id (%d)\n", r);
    }
  } else if (after(line, "shut-down")) {
    if (acpi_has_poweroff()) {
      kprintf("shutting down...\n");
      acpi_poweroff();
      kprintf("shutdown: poweroff failed, halting\n");
    } else {
      kprintf("shutting down (no ACPI, halting)...\n");
    }
    __asm__ volatile ("cli");
    for (;;) { __asm__ volatile ("hlt"); }
  } else {
    kprintf("unknown command: %s (try help)\n", line);
  }
}

#define HIST_N 16

static char g_hist[HIST_N][LINE_MAX];
static int g_hcount; /* entries stored */

/* Erase `n` echoed chars from fb + serial. */
static void erase_chars(size_t n) {
  while (n-- > 0) {
    fb_backspace();
    serial_puts("\b \b");
  }
}

/* Read one line with history (no prompt). Returns length. */
static size_t read_line(char *line) {
  static char saved[LINE_MAX];
  size_t n = 0;
  int nav = -1;
  for (;;) {
    int c;
    /* Caret paint every pass (stateless refresh): input floods, timer
     * quirks and getc waits can never freeze or duplicate it. Live tail
     * only; history view rests. The PIT tick does the same from IRQ. */
    if (fb_is_live()) caret_refresh();
    if (kbd_empty()) {
      __asm__ volatile ("hlt");
      continue;
    }
    c = kbd_getc();
    if (c == KBD_PGUP) {
      fb_scroll_back();
      continue;
    }
    if (c == KBD_PGDN) {
      fb_scroll_fwd();
      continue;
    }
    fb_scroll_live(); /* any other key rejoins the live tail */
    if (c == 0x03) { /* Ctrl+C: cancel the line, fresh prompt */
      erase_chars(n);
      n = 0;
      nav = -1;
      kprintf("^C\n");
      continue;
    }
    if (c == KBD_UP || c == KBD_DOWN) {
      if (g_hcount == 0) continue;
      int oldest = (g_hcount < HIST_N) ? 0 : (g_hcount - HIST_N);
      if (nav == -1 && n > 0) {
        size_t m = 0;
        while (m + 1 < LINE_MAX && m < n) { saved[m] = line[m]; m++; }
        saved[m] = '\0';
      }
      if (c == KBD_UP) {
        nav = (nav == -1) ? (g_hcount - 1)
            : ((nav - 1 < oldest) ? oldest : (nav - 1));
      } else {
        if (nav == -1) continue;
        nav++;
        if (nav >= g_hcount) nav = -1;
      }
      erase_chars(n);
      if (nav == -1) {
        n = 0;
        while (saved[n]) { line[n] = saved[n]; n++; }
      } else {
        const char *src = g_hist[nav % HIST_N];
        n = 0;
        while (src[n] && n + 1 < LINE_MAX) { line[n] = src[n]; n++; }
      }
      for (size_t i = 0; i < n; i++) kprintf("%c", line[i]);
      continue;
    }
    if (c == '\n') {
      kprintf("\n");
      break;
    }
    if (c == '\b') {
      if (n > 0) {
        n--;
        fb_backspace();
        serial_puts("\b \b");
      }
      nav = -1;
      continue;
    }
    if (c == '\t') c = ' '; /* tab -> space */
    if (c < 32 || c > 126) continue; /* printable ASCII only */
    if (n + 1 < LINE_MAX) {
      line[n++] = (char)c;
      kprintf("%c", (char)c);
    }
    nav = -1;
  }
  line[n] = '\0';
  return n;
}

static void remember_line(const char *line, size_t n) {
  if (n == 0) return;
  int last = (g_hcount - 1);
  const char *prev = (last >= 0) ? g_hist[last % HIST_N] : "";
  if (naos_strcmp(prev, line) == 0) return; /* skip dup of last */
  size_t m = 0;
  while (line[m] && m + 1 < LINE_MAX) {
    g_hist[g_hcount % HIST_N][m] = line[m];
    m++;
  }
  g_hist[g_hcount % HIST_N][m] = '\0';
  g_hcount++;
}

void shell_run(void) {
  static char line[LINE_MAX];
  settings_load();
  kprintf("NaOS shell. Type help.\n");
  for (;;) {
    const char *pr = settings_prompt();
    if (g_cwd[0] == '/' && g_cwd[1] == '\0') kprintf("%s> ", pr);
    else kprintf("%s%s> ", pr, g_cwd);
    size_t n = read_line(line);
    remember_line(line, n);
    dispatch(line);
  }
}
