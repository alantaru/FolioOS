/* FOLIO v4 — definições UEFI mínimas (escritas à mão a partir da especificação UEFI 2.x) */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint8_t BOOLEAN; typedef int8_t INT8; typedef uint8_t UINT8;
typedef int16_t INT16; typedef uint16_t UINT16; typedef int32_t INT32; typedef uint32_t UINT32;
typedef int64_t INT64; typedef uint64_t UINT64; typedef uintptr_t UINTN; typedef intptr_t INTN;
typedef uint16_t CHAR16; typedef UINTN EFI_STATUS; typedef void *EFI_HANDLE; typedef void *EFI_EVENT;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
#define EFIAPI
#define EFI_SUCCESS 0
#define EFI_ERR(x) (((UINTN)1 << (sizeof(UINTN) * 8 - 1)) | (x))
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_NOT_READY EFI_ERR(6)
#define EFI_ERROR(s) (((INTN)(s)) < 0)

typedef struct { UINT32 Data1; UINT16 Data2, Data3; UINT8 Data4[8]; } EFI_GUID;
typedef struct { UINT64 Signature; UINT32 Revision, HeaderSize, CRC32, Reserved; } EFI_TABLE_HEADER;
typedef struct { UINT16 Year; UINT8 Month, Day, Hour, Minute, Second, Pad1; UINT32 Nanosecond; INT16 TimeZone; UINT8 Daylight, Pad2; } EFI_TIME;

/* ---- entrada de texto ---- */
typedef struct { UINT16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct { UINT32 KeyShiftState; UINT8 KeyToggleState; } EFI_KEY_STATE;
typedef struct { EFI_INPUT_KEY Key; EFI_KEY_STATE KeyState; } EFI_KEY_DATA;
typedef struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStrokeEx)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *, EFI_KEY_DATA *);
    EFI_EVENT WaitForKeyEx;
    void *SetState, *RegisterKeyNotify, *UnregisterKeyNotify;
} EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;
#define EFI_SHIFT_STATE_VALID 0x80000000
#define EFI_RIGHT_SHIFT 0x01
#define EFI_LEFT_SHIFT 0x02
#define EFI_RIGHT_CONTROL 0x04
#define EFI_LEFT_CONTROL 0x08
#define EFI_RIGHT_ALT 0x10
#define EFI_LEFT_ALT 0x20

/* ---- saída de texto ---- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    void *SetCursorPosition;
    EFI_STATUS (EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- serviços ---- */
typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;
typedef enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData } EFI_MEMORY_TYPE;
typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;
typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;
typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown } EFI_RESET_TYPE;
#define EVT_TIMER 0x80000000

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    void *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    EFI_STATUS (EFIAPI *CreateEvent)(UINT32, UINTN, void *, void *, EFI_EVENT *);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT, EFI_TIMER_DELAY, UINT64);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN, EFI_EVENT *, UINTN *);
    void *SignalEvent;
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT);
    EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT);
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage, *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, UINT64, UINTN, CHAR16 *);
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE, EFI_HANDLE *, void *, BOOLEAN);
    void *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, void *, UINTN *, EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
    void *InstallMultipleProtocolInterfaces, *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32, *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *, void *);
    void *SetTime, *GetWakeupTime, *SetWakeupTime, *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *, EFI_GUID *, UINT32 *, UINTN *, void *);
    void *GetNextVariableName, *SetVariable, *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(EFI_RESET_TYPE, EFI_STATUS, UINTN, void *);
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor; UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle; EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle; EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle; EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries; void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- gráficos ---- */
typedef enum { PixelRedGreenBlueReserved8BitPerColor, PixelBlueGreenRedReserved8BitPerColor, PixelBitMask, PixelBltOnly } EFI_GRAPHICS_PIXEL_FORMAT;
typedef struct { UINT32 RedMask, GreenMask, BlueMask, ReservedMask; } EFI_PIXEL_BITMASK;
typedef struct {
    UINT32 Version, HorizontalResolution, VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat; EFI_PIXEL_BITMASK PixelInformation; UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct {
    UINT32 MaxMode, Mode; EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info; UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase; UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
typedef enum { EfiBltVideoFill, EfiBltVideoToBltBuffer, EfiBltBufferToVideo, EfiBltVideoToVideo } EFI_GRAPHICS_OUTPUT_BLT_OPERATION;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32, UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32);
    EFI_STATUS (EFIAPI *Blt)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, void *, EFI_GRAPHICS_OUTPUT_BLT_OPERATION, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef struct { UINT32 SizeOfEdid; UINT8 *Edid; } EFI_EDID_ACTIVE_PROTOCOL;
/* UGA Draw (EFI 1.1: Macs Intel antigos e firmwares de ~2006–2009 sem GOP) */
typedef enum { EfiUgaVideoFill, EfiUgaVideoToBltBuffer, EfiUgaBltBufferToVideo, EfiUgaVideoToVideo } EFI_UGA_BLT_OPERATION;
typedef struct EFI_UGA_DRAW_PROTOCOL {
    EFI_STATUS (EFIAPI *GetMode)(struct EFI_UGA_DRAW_PROTOCOL *, UINT32 *, UINT32 *, UINT32 *, UINT32 *);
    EFI_STATUS (EFIAPI *SetMode)(struct EFI_UGA_DRAW_PROTOCOL *, UINT32, UINT32, UINT32, UINT32);
    EFI_STATUS (EFIAPI *Blt)(struct EFI_UGA_DRAW_PROTOCOL *, void *, EFI_UGA_BLT_OPERATION, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN);
} EFI_UGA_DRAW_PROTOCOL;

/* ---- ponteiros ---- */
typedef struct { INT32 RelativeMovementX, RelativeMovementY, RelativeMovementZ; BOOLEAN LeftButton, RightButton; } EFI_SIMPLE_POINTER_STATE;
typedef struct { UINT64 ResolutionX, ResolutionY, ResolutionZ; BOOLEAN LeftButton, RightButton; } EFI_SIMPLE_POINTER_MODE;
typedef struct EFI_SIMPLE_POINTER_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_POINTER_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *GetState)(struct EFI_SIMPLE_POINTER_PROTOCOL *, EFI_SIMPLE_POINTER_STATE *);
    EFI_EVENT WaitForInput;
    EFI_SIMPLE_POINTER_MODE *Mode;
} EFI_SIMPLE_POINTER_PROTOCOL;
typedef struct { UINT64 CurrentX, CurrentY, CurrentZ; UINT32 ActiveButtons; } EFI_ABSOLUTE_POINTER_STATE;
typedef struct { UINT64 AbsoluteMinX, AbsoluteMinY, AbsoluteMinZ, AbsoluteMaxX, AbsoluteMaxY, AbsoluteMaxZ; UINT32 Attributes; } EFI_ABSOLUTE_POINTER_MODE;
typedef struct EFI_ABSOLUTE_POINTER_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *GetState)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *, EFI_ABSOLUTE_POINTER_STATE *);
    EFI_EVENT WaitForInput;
    EFI_ABSOLUTE_POINTER_MODE *Mode;
} EFI_ABSOLUTE_POINTER_PROTOCOL;

/* ---- imagem e arquivos ---- */
typedef struct {
    UINT32 Revision; EFI_HANDLE ParentHandle; EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle; void *FilePath; void *Reserved;
    UINT32 LoadOptionsSize; void *LoadOptions; void *ImageBase; UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType, ImageDataType; void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **, CHAR16 *, UINT64, UINT64);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *Delete)(struct EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (EFIAPI *Write)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *, UINT64 *);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *, UINT64);
    EFI_STATUS (EFIAPI *GetInfo)(struct EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, void *);
    EFI_STATUS (EFIAPI *SetInfo)(struct EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (EFIAPI *Flush)(struct EFI_FILE_PROTOCOL *);
} EFI_FILE_PROTOCOL;
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct {
    UINT64 Size, FileSize, PhysicalSize;
    EFI_TIME CreateTime, LastAccessTime, ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;
typedef struct {
    UINT64 Size; BOOLEAN ReadOnly; UINT64 VolumeSize, FreeSpace; UINT32 BlockSize;
    CHAR16 VolumeLabel[1];
} EFI_FILE_SYSTEM_INFO;
#define EFI_FILE_MODE_READ 0x1ull
#define EFI_FILE_MODE_WRITE 0x2ull
#define EFI_FILE_MODE_CREATE 0x8000000000000000ull
#define EFI_FILE_DIRECTORY 0x10ull

/* ---- PCI I/O (acesso a dispositivos PCI pelo firmware: config, DMA mapeado pela IOMMU) ---- */
typedef enum { EfiPciIoWidthUint8, EfiPciIoWidthUint16, EfiPciIoWidthUint32, EfiPciIoWidthUint64 } EFI_PCI_IO_PROTOCOL_WIDTH;
typedef enum { EfiPciIoOperationBusMasterRead, EfiPciIoOperationBusMasterWrite, EfiPciIoOperationBusMasterCommonBuffer } EFI_PCI_IO_PROTOCOL_OPERATION;
typedef enum { EfiPciIoAttributeOperationGet, EfiPciIoAttributeOperationSet, EfiPciIoAttributeOperationEnable,
               EfiPciIoAttributeOperationDisable, EfiPciIoAttributeOperationSupported } EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION;
#define EFI_PCI_IO_ATTRIBUTE_IO 0x0001
#define EFI_PCI_IO_ATTRIBUTE_MEMORY 0x0002
#define EFI_PCI_IO_ATTRIBUTE_BUS_MASTER 0x0004
#define EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE 0x8000
struct EFI_PCI_IO_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_PCI_IO_MEM)(struct EFI_PCI_IO_PROTOCOL *, EFI_PCI_IO_PROTOCOL_WIDTH, UINT8, UINT64, UINTN, void *);
typedef EFI_STATUS (EFIAPI *EFI_PCI_IO_CFG)(struct EFI_PCI_IO_PROTOCOL *, EFI_PCI_IO_PROTOCOL_WIDTH, UINT32, UINTN, void *);
typedef struct EFI_PCI_IO_PROTOCOL {
    void *PollMem, *PollIo;
    struct { EFI_PCI_IO_MEM Read, Write; } Mem, Io;
    struct { EFI_PCI_IO_CFG Read, Write; } Pci;
    void *CopyMem;
    EFI_STATUS (EFIAPI *Map)(struct EFI_PCI_IO_PROTOCOL *, EFI_PCI_IO_PROTOCOL_OPERATION, void *, UINTN *, EFI_PHYSICAL_ADDRESS *, void **);
    EFI_STATUS (EFIAPI *Unmap)(struct EFI_PCI_IO_PROTOCOL *, void *);
    EFI_STATUS (EFIAPI *AllocateBuffer)(struct EFI_PCI_IO_PROTOCOL *, EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, void **, UINT64);
    EFI_STATUS (EFIAPI *FreeBuffer)(struct EFI_PCI_IO_PROTOCOL *, UINTN, void *);
    void *Flush;
    EFI_STATUS (EFIAPI *GetLocation)(struct EFI_PCI_IO_PROTOCOL *, UINTN *, UINTN *, UINTN *, UINTN *);
    EFI_STATUS (EFIAPI *Attributes)(struct EFI_PCI_IO_PROTOCOL *, EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION, UINT64, UINT64 *);
    void *GetBarAttributes, *SetBarAttributes;
    UINT64 RomSize; void *RomImage;
} EFI_PCI_IO_PROTOCOL;

/* globais exportados por uefi_main.c para os drivers */
extern EFI_SYSTEM_TABLE *efi_st;
extern EFI_BOOT_SERVICES *efi_bs;
