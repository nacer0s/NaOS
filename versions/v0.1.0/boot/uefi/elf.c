/* NaOS ELF64 loader. Standard ELF64 structs, own parsing. x86-64, LE, exec. */
#include "elf.h"
#include "../../common/cstring.h"

typedef struct {
  uint8_t  e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} Elf64_Phdr;

#define PT_LOAD 1u
#define EM_X86_64 62u

EFI_STATUS elf_load(EFI_BOOT_SERVICES *bs, const uint8_t *img, UINTN size,
                    EFI_PHYSICAL_ADDRESS *entry_out) {
  if (size < sizeof(Elf64_Ehdr)) return 100;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
  if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
      eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return 101;
  if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1) return 102; /* 64-bit LE */
  if (eh->e_machine != EM_X86_64) return 103;
  if (eh->e_phentsize != sizeof(Elf64_Phdr)) return 104;
  if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > size) return 105;

  const Elf64_Phdr *ph = (const Elf64_Phdr *)(img + eh->e_phoff);
  for (uint16_t i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD) continue;
    if (ph[i].p_memsz == 0) continue;
    if (ph[i].p_offset + ph[i].p_filesz > size) return 106;
    if (ph[i].p_paddr < 0x100000ULL) return 107; /* refuse low-memory clobber */

    uint64_t pages = (ph[i].p_memsz + 0xFFFULL) / 0x1000ULL;
    EFI_PHYSICAL_ADDRESS at = ph[i].p_paddr;
    EFI_STATUS st = bs->AllocatePages(AllocateAddress, EfiLoaderData,
                                     (UINTN)pages, &at);
    if (st != EFI_SUCCESS || at != ph[i].p_paddr) return 108;

    uint8_t *dst = (uint8_t *)(uintptr_t)at;
    naos_memcpy(dst, img + ph[i].p_offset, (size_t)ph[i].p_filesz);
    if (ph[i].p_memsz > ph[i].p_filesz)
      naos_memset(dst + ph[i].p_filesz, 0,
                  (size_t)(ph[i].p_memsz - ph[i].p_filesz));
  }

  *entry_out = (EFI_PHYSICAL_ADDRESS)eh->e_entry;
  if (*entry_out < 0x100000ULL) return 109;
  return EFI_SUCCESS;
}
