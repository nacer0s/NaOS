/* NaOS boot/uefi/uefi.h — OWN minimal UEFI definitions, hand-written from the UEFI spec.
 * No gnu-efi, no EDK2. Covers: ConOut, BootServices (positional — field ORDER
 * must match the spec), LoadedImage, SimpleFileSystem+File, GOP, MemoryMap. */
#ifndef NAOS_UEFI_H
#define NAOS_UEFI_H

#include <stdint.h>

/* ---- Basic types ---- */
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int64_t   INT64;
typedef uint64_t  UINTN;      /* 64-bit on x86_64 */
typedef int64_t   INTN;
typedef uint16_t  CHAR16;     /* UCS-2 */
typedef void      VOID;
typedef UINTN     EFI_STATUS;
typedef VOID     *EFI_HANDLE;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;

#define EFI_SUCCESS 0
#define EFI_ERROR_BIT 0x8000000000000000ULL
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)  /* high bit set on real firmware */

/* Calling convention: Microsoft x64 ABI — default for mingw x86_64. */

typedef struct {
  UINT32 Data1;
  UINT16 Data2;
  UINT16 Data3;
  UINT8  Data4[8];
} EFI_GUID;

typedef struct {
  UINT64 Signature;
  UINT32 Revision;
  UINT32 HeaderSize;
  UINT32 CRC32;
  UINT32 Reserved;
} EFI_TABLE_HEADER;   /* 24 bytes — NOT a single pointer! */

/* ---- ConOut ---- */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (*EFI_TEXT_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                     const CHAR16 *String);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
  VOID *Reset;
  EFI_TEXT_STRING OutputString;
  VOID *TestString;
  VOID *QueryMode;
  VOID *SetMode;
  VOID *SetAttribute;
  VOID *ClearScreen;
  VOID *SetCursorPosition;
  VOID *EnableCursor;
  VOID *Mode;
};

/* ---- Memory ---- */
typedef enum {
  EfiReservedMemoryType = 0,
  EfiLoaderCode = 1,
  EfiLoaderData = 2,
  EfiBootServicesCode = 3,
  EfiBootServicesData = 4,
  EfiRuntimeServicesCode = 5,
  EfiRuntimeServicesData = 6,
  EfiConventionalMemory = 7,
  EfiUnusableMemory = 8,
  EfiACPIReclaimMemory = 9,
  EfiACPIMemoryNVS = 10,
  EfiMemoryMappedIO = 11,
  EfiMemoryMappedIOPortSpace = 12,
  EfiPalCode = 13,
  EfiPersistentMemory = 14,
  EfiMaxMemoryType = 15
} EFI_MEMORY_TYPE;

typedef enum {
  AllocateAnyPages = 0,
  AllocateMaxAddress = 1,
  AllocateAddress = 2,
  MaxAllocateType = 3
} EFI_ALLOCATE_TYPE;

typedef struct {
  UINT32 Type;
  UINT32 Pad;
  EFI_PHYSICAL_ADDRESS PhysicalStart;
  EFI_VIRTUAL_ADDRESS  VirtualStart;
  UINT64 NumberOfPages;
  UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- BootServices (POSITIONAL — keep spec order, use VOID* for unused) ---- */
struct _EFI_BOOT_SERVICES;
typedef struct _EFI_BOOT_SERVICES EFI_BOOT_SERVICES;

typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(int /*EFI_ALLOCATE_TYPE*/ Type,
                                        int /*EFI_MEMORY_TYPE*/ MemoryType,
                                        UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (*EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(UINTN *MemoryMapSize,
                                        EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                        UINTN *MapKey, UINTN *DescriptorSize,
                                        UINT32 *DescriptorVersion);
typedef EFI_STATUS (*EFI_ALLOCATE_POOL)(int /*EFI_MEMORY_TYPE*/ PoolType,
                                       UINTN Size, VOID **Buffer);
typedef EFI_STATUS (*EFI_FREE_POOL)(VOID *Buffer);
typedef EFI_STATUS (*EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle,
                                         const EFI_GUID *Protocol,
                                         VOID **Interface);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(const EFI_GUID *Protocol,
                                         VOID *Registration, VOID **Interface);
typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (*EFI_STALL)(UINTN Microseconds);

struct _EFI_BOOT_SERVICES {
  EFI_TABLE_HEADER Hdr;
  VOID *RaiseTPL;
  VOID *RestoreTPL;
  EFI_ALLOCATE_PAGES AllocatePages;
  EFI_FREE_PAGES FreePages;
  EFI_GET_MEMORY_MAP GetMemoryMap;
  EFI_ALLOCATE_POOL AllocatePool;
  EFI_FREE_POOL FreePool;
  VOID *CreateEvent;
  VOID *SetTimer;
  VOID *WaitForEvent;
  VOID *SignalEvent;
  VOID *CloseEvent;
  VOID *CheckEvent;
  VOID *InstallProtocolInterface;
  VOID *ReinstallProtocolInterface;
  VOID *UninstallProtocolInterface;
  EFI_HANDLE_PROTOCOL HandleProtocol;
  VOID *Reserved;
  VOID *RegisterProtocolNotify;
  VOID *LocateHandle;
  VOID *LocateDevicePath;
  VOID *InstallConfigurationTable;
  VOID *LoadImage;
  VOID *StartImage;
  VOID *Exit;
  VOID *UnloadImage;
  EFI_EXIT_BOOT_SERVICES ExitBootServices;
  VOID *GetNextMonotonicCount;
  EFI_STALL Stall;
  VOID *SetWatchdogTimer;
  VOID *ConnectController;
  VOID *DisconnectController;
  VOID *OpenProtocol;
  VOID *CloseProtocol;
  VOID *OpenProtocolInformation;
  VOID *ProtocolsPerHandle;
  VOID *LocateHandleBuffer;
  EFI_LOCATE_PROTOCOL LocateProtocol;   /* enough for GOP via first handle */
  VOID *InstallMultipleProtocolInterfaces;
  VOID *UninstallMultipleProtocolInterfaces;
  VOID *CalculateCrc32;
  VOID *CopyMem;
  VOID *SetMem;
  VOID *CreateEventEx;
};

/* ---- LoadedImage (to find our ESP device handle) ---- */
typedef struct {
  UINT32 Revision;
  EFI_HANDLE ParentHandle;
  VOID *SystemTable;
  EFI_HANDLE DeviceHandle;
  VOID *FilePath;
  VOID *Reserved;
  UINT32 LoadOptionsSize;
  VOID *LoadOptions;
  VOID *ImageBase;
  UINT64 ImageSize;
  int CodeType;
  int DataType;
  VOID *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- File system ---- */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (*EFI_VOLUME_OPEN)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                     EFI_FILE_PROTOCOL **Root);
typedef EFI_STATUS (*EFI_FILE_OPEN)(EFI_FILE_PROTOCOL *This,
                                   EFI_FILE_PROTOCOL **NewHandle,
                                   const CHAR16 *FileName, UINT64 OpenMode,
                                   UINT64 Attributes);
typedef EFI_STATUS (*EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (*EFI_FILE_READ)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize,
                                   VOID *Buffer);
typedef EFI_STATUS (*EFI_FILE_GETINFO)(EFI_FILE_PROTOCOL *This,
                                      const EFI_GUID *InformationType,
                                      UINTN *BufferSize, VOID *Buffer);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
  UINT64 Revision;
  EFI_VOLUME_OPEN OpenVolume;
};

struct _EFI_FILE_PROTOCOL {
  UINT64 Revision;
  EFI_FILE_OPEN Open;
  EFI_FILE_CLOSE Close;
  VOID *Delete;
  EFI_FILE_READ Read;
  VOID *Write;
  VOID *GetPosition;
  VOID *SetPosition;
  EFI_FILE_GETINFO GetInfo;
  VOID *SetInfo;
  VOID *Flush;
};

#define EFI_FILE_MODE_READ 1ULL

typedef struct {
  UINT64 Size;
  UINT64 FileSize;
  UINT64 PhysicalSize;
  UINT8  CreateTime[16];
  UINT8  LastAccessTime[16];
  UINT8  ModificationTime[16];
  UINT64 Attribute;
  CHAR16 FileName[1];
} EFI_FILE_INFO;

/* ---- GOP ---- */
typedef struct {
  UINT8 Blue;
  UINT8 Green;
  UINT8 Red;
  UINT8 Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum {
  PixelRedGreenBlueReserved8BitPerColor = 0,
  PixelBlueGreenRedReserved8BitPerColor = 1,
  PixelBitMask = 2,
  PixelBltOnly = 3,
  PixelFormatMax = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
  UINT32 RedMask;
  UINT32 GreenMask;
  UINT32 BlueMask;
  UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
  UINT32 Version;
  UINT32 HorizontalResolution;
  UINT32 VerticalResolution;
  EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
  EFI_PIXEL_BITMASK PixelInformation;
  UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
  UINT32 MaxMode;
  UINT32 Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  UINTN SizeOfInfo;
  EFI_PHYSICAL_ADDRESS FrameBufferBase;
  UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef EFI_STATUS (*EFI_GOP_QUERY_MODE)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                                        UINT32 ModeNumber, UINTN *SizeOfInfo,
                                        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
typedef EFI_STATUS (*EFI_GOP_SET_MODE)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                                      UINT32 ModeNumber);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
  EFI_GOP_QUERY_MODE QueryMode;
  EFI_GOP_SET_MODE SetMode;
  VOID *Blt;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* ---- SystemTable ---- */
typedef struct {
  EFI_GUID VendorGuid;
  VOID *VendorTable;
} EFI_CONFIGURATION_TABLE;

static const EFI_GUID kEfiAcpi20TableGuid =
  {0x8868E871, 0xE4F1, 0x11D3, {0xBC,0x22,0x00,0x80,0xC7,0x3C,0x88,0x81}};
static const EFI_GUID kEfiAcpiTableGuid =
  {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A,0x16,0x00,0x90,0x27,0x3F,0xC1,0x4D}};

typedef struct {
  EFI_TABLE_HEADER Hdr;
  CHAR16 *FirmwareVendor;
  UINT32 FirmwareRevision;
  EFI_HANDLE ConsoleInHandle;
  VOID *ConIn;
  EFI_HANDLE ConsoleOutHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_HANDLE StandardErrorHandle;
  VOID *StdErr;
  VOID *RuntimeServices;
  EFI_BOOT_SERVICES *BootServices;
  UINTN NumberOfTableEntries;
  EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Well-known GUIDs ---- */
static const EFI_GUID kEfiLoadedImageProtocolGuid =
  {0x5B1B31A1, 0x9562, 0x11d2, {0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID kEfiSimpleFileSystemProtocolGuid =
  {0x0964e5b22, 0x6459, 0x11d2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID kEfiFileInfoGuid =
  {0x09576e92, 0x6d3f, 0x11d2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID kEfiGraphicsOutputProtocolGuid =
  {0x9042a9de, 0x23dc, 0x4a38, {0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};

#endif /* NAOS_UEFI_H */
