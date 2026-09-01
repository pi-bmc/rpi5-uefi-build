#ifndef USB_EEM_TEST_BASEMEMORYLIB_H_
#define USB_EEM_TEST_BASEMEMORYLIB_H_

#include <string.h>
#include <Uefi.h>

static inline VOID *
CopyMem (VOID *Dest, CONST VOID *Src, UINTN Len)
{
  return memcpy (Dest, Src, Len);
}

#endif
