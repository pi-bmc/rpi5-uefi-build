// Minimal EDK2 surface for compiling UsbEemFraming.c on the build host.
// Deliberately NOT an ifdef inside the driver: shipped sources include the
// real <Uefi.h>, and the test just puts these earlier on the include path.
#ifndef USB_EEM_TEST_UEFI_H_
#define USB_EEM_TEST_UEFI_H_

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef size_t   UINTN;
typedef int      BOOLEAN;

#define CONST const
#define IN
#define OUT
#define VOID void

typedef UINTN EFI_STATUS;

#define EFI_SUCCESS            0
#define EFI_INVALID_PARAMETER  2
#define EFI_BUFFER_TOO_SMALL   5
#define EFI_NOT_FOUND          14

#define BIT14 0x00004000
#define BIT15 0x00008000

#endif
