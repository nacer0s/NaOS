/* NaOS bootloader file loader. Flow: LoadedImage -> DeviceHandle -> SimpleFS
 * -> OpenVolume -> Open(path) -> GetInfo(size) -> AllocatePool -> Read. */
#include "file.h"

EFI_STATUS file_read_all(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                         const CHAR16 *path, uint8_t **buf_out, UINTN *size_out) {
  EFI_STATUS st;
  EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
  EFI_FILE_PROTOCOL *root = 0, *file = 0;
  VOID *buf = 0;

  *buf_out = 0;
  *size_out = 0;

  st = bs->HandleProtocol(image, &kEfiLoadedImageProtocolGuid, (VOID **)&loaded);
  if (st != EFI_SUCCESS || !loaded) return st ? st : 1;

  st = bs->HandleProtocol(loaded->DeviceHandle, &kEfiSimpleFileSystemProtocolGuid,
                          (VOID **)&fs);
  if (st != EFI_SUCCESS || !fs) return st ? st : 2;

  st = fs->OpenVolume(fs, &root);
  if (st != EFI_SUCCESS || !root) return st ? st : 3;

  st = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
  root->Close(root);
  if (st != EFI_SUCCESS || !file) return st ? st : 4;

  /* Query size via FileInfo. */
  UINTN info_size = sizeof(EFI_FILE_INFO) + 256;
  EFI_FILE_INFO *info = 0;
  st = bs->AllocatePool(EfiLoaderData, info_size, (VOID **)&info);
  if (st != EFI_SUCCESS) { file->Close(file); return st; }
  st = file->GetInfo(file, &kEfiFileInfoGuid, &info_size, info);
  if (st == EFI_BUFFER_TOO_SMALL) {
    bs->FreePool(info);
    st = bs->AllocatePool(EfiLoaderData, info_size, (VOID **)&info);
    if (st != EFI_SUCCESS) { file->Close(file); return st; }
    st = file->GetInfo(file, &kEfiFileInfoGuid, &info_size, info);
  }
  if (st != EFI_SUCCESS) {
    bs->FreePool(info);
    file->Close(file);
    return st;
  }

  UINTN fsize = (UINTN)info->FileSize;
  bs->FreePool(info);
  if (fsize == 0 || fsize > (128u * 1024u * 1024u)) { file->Close(file); return 5; }

  st = bs->AllocatePool(EfiLoaderData, fsize, &buf);
  if (st != EFI_SUCCESS) { file->Close(file); return st; }

  UINTN left = fsize;
  st = file->Read(file, &left, buf);
  file->Close(file);
  if (st != EFI_SUCCESS || left != fsize) {
    bs->FreePool(buf);
    return st ? st : 6;
  }

  *buf_out = (uint8_t *)buf;
  *size_out = fsize;
  return EFI_SUCCESS;
}
