#ifndef EFI_H
#define EFI_H

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef long long          INT64;

typedef UINT64             UINTN;
typedef INT64              INTN;
typedef UINT16             CHAR16;
typedef UINT8              BOOLEAN;
typedef void               VOID;

typedef UINTN              EFI_STATUS;
typedef VOID*              EFI_HANDLE;
typedef VOID*              EFI_EVENT;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS 0

/* ============================================================
 * GUID
 * ============================================================ */

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* ============================================================
 * Time
 * ============================================================ */

typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT64  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

/* ============================================================
 * Table header
 * ============================================================ */

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ============================================================
 * Simple Text Output Protocol
 * ============================================================ */

typedef struct SIMPLE_TEXT_OUTPUT_INTERFACE
    SIMPLE_TEXT_OUTPUT_INTERFACE;

typedef struct {
    INTN MaxMode;
    INTN Mode;
    INTN Attribute;
    INTN CursorColumn;
    INTN CursorRow;
    BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    BOOLEAN ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    SIMPLE_TEXT_OUTPUT_INTERFACE *This
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    SIMPLE_TEXT_OUTPUT_INTERFACE *This,
    UINTN Attribute
);

struct SIMPLE_TEXT_OUTPUT_INTERFACE {
    EFI_TEXT_RESET           Reset;
    EFI_TEXT_STRING          OutputString;
    VOID                    *TestString;
    VOID                    *QueryMode;
    VOID                    *SetMode;
    EFI_TEXT_SET_ATTRIBUTE   SetAttribute;
    EFI_TEXT_CLEAR_SCREEN    ClearScreen;
    VOID                    *SetCursorPosition;
    VOID                    *EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE *Mode;
};

/* Alias, чтобы оба имени работали */
typedef SIMPLE_TEXT_OUTPUT_INTERFACE EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ============================================================
 * Simple Text Input Protocol
 * ============================================================ */

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct SIMPLE_INPUT_INTERFACE SIMPLE_INPUT_INTERFACE;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(
    SIMPLE_INPUT_INTERFACE *This,
    BOOLEAN ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(
    SIMPLE_INPUT_INTERFACE *This,
    EFI_INPUT_KEY *Key
);

struct SIMPLE_INPUT_INTERFACE {
    EFI_INPUT_RESET     Reset;
    EFI_INPUT_READ_KEY  ReadKeyStroke;
    EFI_EVENT           WaitForKey;
};

/* ============================================================
 * Graphics Output Protocol
 * ============================================================ */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    UINT32 PixelInformation[4];
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    UINT64 FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL
    EFI_GRAPHICS_OUTPUT_PROTOCOL;

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    VOID *QueryMode;
    VOID *SetMode;
    VOID *Blt;

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, \
      { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

/* ============================================================
 * Boot Services
 *
 * До LocateProtocol идут 16 указателей.
 * ============================================================ */

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol,
    VOID *Registration,
    VOID **Interface
);

typedef struct {
    EFI_TABLE_HEADER Hdr;

    VOID *RaiseTPL;
    VOID *RestoreTPL;

    VOID *AllocatePages;
    VOID *FreePages;
    VOID *GetMemoryMap;
    VOID *AllocatePool;
    VOID *FreePool;

    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;

    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    VOID *HandleProtocol;
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    EFI_LOCATE_PROTOCOL LocateProtocol;

    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;

    VOID *CalculateCrc32;
    VOID *CopyMem;
    VOID *SetMem;
    VOID *CreateEventEx;

    UINT64 (EFIAPI *Stall)(UINTN Microseconds);

} EFI_BOOT_SERVICES;

/* ============================================================
 * Runtime Services
 * ============================================================ */

typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
    EfiResetPlatformSpecific
} EFI_RESET_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_GET_TIME)(
    EFI_TIME *Time,
    VOID *Capabilities
);

typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(
    CHAR16 *VariableName,
    EFI_GUID *VendorGuid,
    UINT32 *Attributes,
    UINTN *DataSize,
    VOID *Data
);

typedef struct {
    EFI_TABLE_HEADER Hdr;

    EFI_GET_TIME GetTime;

    VOID *SetTime;
    VOID *GetWakeupTime;
    VOID *SetWakeupTime;
    VOID *SetVirtualAddressMap;
    VOID *ConvertPointer;

    EFI_GET_VARIABLE GetVariable;

    VOID *GetNextVariableName;
    VOID *SetVariable;
    VOID *GetNextHighMonotonicCount;

    VOID (EFIAPI *ResetSystem)(
        EFI_RESET_TYPE ResetType,
        EFI_STATUS ResetStatus,
        UINTN DataSize,
        VOID *ResetData
    );

    VOID *UpdateCapsule;
    VOID *QueryCapsuleCapabilities;
    VOID *QueryVariableInfo;

} EFI_RUNTIME_SERVICES;

/* ============================================================
 * System Table
 * ============================================================ */

typedef struct {
    EFI_TABLE_HEADER Hdr;

    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;

    EFI_HANDLE ConsoleInHandle;
    SIMPLE_INPUT_INTERFACE *ConIn;

    EFI_HANDLE ConsoleOutHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut;

    EFI_HANDLE StandardErrorHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE *StdErr;

    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;

    UINTN NumberOfTableEntries;
    VOID *ConfigurationTable;

} EFI_SYSTEM_TABLE;

/* ============================================================
 * Characters
 * ============================================================ */

#define CHAR_BACKSPACE       0x0008
#define CHAR_CARRIAGE_RETURN 0x000D

#endif
