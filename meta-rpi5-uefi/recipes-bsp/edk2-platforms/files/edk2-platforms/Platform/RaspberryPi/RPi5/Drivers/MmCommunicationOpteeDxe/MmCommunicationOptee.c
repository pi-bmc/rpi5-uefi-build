/** @file
  EFI_MM_COMMUNICATION2_PROTOCOL over the OP-TEE StandaloneMM (StMM) SP.

  Relays the MM communication buffer to OP-TEE's StMM secure partition, which
  OP-TEE runs as an ordinary pseudo-TA under the opteed dispatcher (no FF-A
  SPMC). Session setup reuses ArmPkg/OpteeLib (proven on this board by
  RpiOpteeSensorDxe); the OPEN_SESSION and InvokeCommand calls are built here
  so the OP-TEE normal-world RPC loop (shared-memory alloc/free, foreign
  interrupts) is serviced -- OpteeLib's loop drops RPCs.

  Storage involves no RPCs at all: StMM's variable and FTW drivers work
  directly on the VPU-loaded FD NV window OP-TEE maps into the SP
  (CFG_STMM_VARSTORE_*, RpiNvMemFvb). This driver persists that window to
  the boot FAT (VarStoreSync.c), marking it dirty on every successful
  SetVariable communicate.

  Runtime: DXE_RUNTIME_DRIVER so the protocol stays callable after
  ExitBootServices, where it refuses cleanly (EFI_UNSUPPORTED) -- OS-runtime
  variable access needs the OP-TEE SHM window in the runtime memory map and
  an OS-side flush path, which is staged work, not silent breakage.

  Copyright (c) 2026, pi-bmc.  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/OpteeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Guid/EventGroup.h>
#include <Guid/SmmVariableCommon.h>
#include <Protocol/MmCommunication2.h>
#include <Protocol/SmmVariable.h>

#include "MmCommunicationOptee.h"

//
// Cached OP-TEE static shared memory window (from GET_SHM_CONFIG) and the
// per-call bump allocator over it. The window is the plat-rpi5 reserved SHM
// (0x1F000000); OpteeInit() already remapped it WB and validated it.
//
STATIC UINTN    mShmBase     = 0;
STATIC UINTN    mShmSize     = 0;
STATIC UINTN    mShmNext     = 0; // bump cursor, reset at each Communicate
STATIC UINT32   mSession     = 0;
STATIC BOOLEAN  mHaveSession = FALSE;
STATIC BOOLEAN  mAtRuntime   = FALSE;

#define SHM_ALIGN_UP(x)  (((x) + 0xFUL) & ~0xFUL)

/**
  Read the OP-TEE static SHM window base/size (matches what OpteeInit mapped).
**/
STATIC
EFI_STATUS
MmOpteeGetShm (
  VOID
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Start;
  UINTN         End;

  if (mShmBase != 0) {
    return EFI_SUCCESS;
  }

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_GET_SHM_CONFIG;
  ArmCallSmc (&Args);
  if (Args.Arg0 != OPTEE_SMC_RETURN_OK) {
    return EFI_UNSUPPORTED;
  }

  Start = (Args.Arg1 + SIZE_4KB - 1) & ~(UINTN)(SIZE_4KB - 1);
  End   = (Args.Arg1 + Args.Arg2) & ~(UINTN)(SIZE_4KB - 1);
  if (End <= Start) {
    return EFI_BUFFER_TOO_SMALL;
  }

  mShmBase = Start;
  mShmSize = End - Start;
  return EFI_SUCCESS;
}

/**
  Bump-allocate Size bytes from the SHM window. Returns 0 on exhaustion.
  Cookies handed to OP-TEE are the buffer's physical address (identity mapped).
**/
STATIC
UINTN
MmOpteeShmAlloc (
  IN UINTN  Size
  )
{
  UINTN  Addr;

  Addr = mShmNext;
  if ((Size == 0) || (SHM_ALIGN_UP (Size) > (mShmSize - (Addr - mShmBase)))) {
    return 0;
  }

  mShmNext = Addr + SHM_ALIGN_UP (Size);
  return Addr;
}

/**
  Service one OPTEE_RPC_CMD_* request (delivered via OPTEE_SMC_RPC_FUNC_CMD).
  MsgArg lives in SHM and is updated in place with the result.
**/
STATIC
VOID
MmOpteeRpcCmd (
  IN OUT OPTEE_MSG_ARG  *MsgArg
  )
{
  UINTN   Buf;
  UINT64  Size;

  switch (MsgArg->Cmd) {
    case OPTEE_RPC_CMD_SHM_ALLOC:
      //
      // [in] value[0].b = size, value[0].c = align; [out] memref[0] = buffer.
      //
      if (MsgArg->NumParams != 1) {
        MsgArg->Ret = TEE_ERROR_GENERIC;
        break;
      }

      Size = MsgArg->Params[0].U.Value.B;
      Buf  = MmOpteeShmAlloc ((UINTN)Size);
      if (Buf == 0) {
        MsgArg->Ret = TEE_ERROR_OUT_OF_MEMORY;
        break;
      }

      MsgArg->Params[0].Attr          = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
      MsgArg->Params[0].U.TMem.BufPtr = (UINT64)Buf;
      MsgArg->Params[0].U.TMem.Size   = Size;
      MsgArg->Params[0].U.TMem.ShmRef = (UINT64)Buf;   // cookie = PA
      MsgArg->Ret                     = TEE_SUCCESS;
      break;

    case OPTEE_RPC_CMD_SHM_FREE:
      //
      // Bump allocator reclaims wholesale at the next Communicate; nothing to
      // do per free.
      //
      MsgArg->Ret = TEE_SUCCESS;
      break;

    //
    // No storage RPCs are expected any more: StMM's variable stack works on
    // the mapped FD NV window, and OP-TEE's storage service / REE-FS / RPMB
    // paths sit unused. Anything else is answered, not dropped, so OP-TEE
    // fails the operation cleanly instead of hanging.
    //
    default:
      DEBUG ((DEBUG_WARN, "%a: unhandled OP-TEE RPC cmd %u\n", __func__, MsgArg->Cmd));
      MsgArg->Ret = TEE_ERROR_NOT_SUPPORTED;
      break;
  }
}

/**
  Issue OPTEE_SMC_CALL_WITH_ARG for the message at ArgPa and run the OP-TEE
  normal-world RPC loop until the call completes. Returns the OP-TEE SMC
  return value (0 = OK).
**/
STATIC
UINT32
MmOpteeCallWithRpc (
  IN UINTN  ArgPa
  )
{
  ARM_SMC_ARGS  Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_CALL_WITH_ARG;
  Args.Arg1 = (UINT32)(((UINT64)ArgPa) >> 32);
  Args.Arg2 = (UINT32)ArgPa;

  while (TRUE) {
    ArmCallSmc (&Args);

    if (!OPTEE_SMC_RETURN_IS_RPC (Args.Arg0)) {
      break;
    }

    switch (OPTEE_SMC_RETURN_GET_RPC_FUNC (Args.Arg0)) {
      case OPTEE_SMC_RPC_FUNC_ALLOC:
      {
        UINTN  Buf;

        Buf = MmOpteeShmAlloc ((UINTN)Args.Arg1);
        //
        // a1:a2 = 64-bit physical pointer, a4:a5 = cookie (= PA). All zero
        // signals allocation failure, which OP-TEE handles gracefully.
        //
        Args.Arg1 = (UINT32)(((UINT64)Buf) >> 32);
        Args.Arg2 = (UINT32)Buf;
        Args.Arg4 = (UINT32)(((UINT64)Buf) >> 32);
        Args.Arg5 = (UINT32)Buf;
        break;
      }

      case OPTEE_SMC_RPC_FUNC_FREE:
        //
        // Cookie in a1:a2; bump allocator reclaims at the next Communicate.
        //
        break;

      case OPTEE_SMC_RPC_FUNC_FOREIGN_INTR:
        //
        // A foreign (non-secure) interrupt fired while OP-TEE ran. It is
        // taken through the normal vector by returning to OP-TEE; nothing to
        // do here.
        //
        break;

      case OPTEE_SMC_RPC_FUNC_CMD:
      {
        UINT64  Cookie;

        //
        // a1:a2 = cookie of a shared-memory buffer holding an OPTEE_MSG_ARG.
        // The cookie is the buffer PA (see MmOpteeShmAlloc), identity mapped.
        //
        Cookie = (((UINT64)Args.Arg1) << 32) | (UINT32)Args.Arg2;
        MmOpteeRpcCmd ((OPTEE_MSG_ARG *)(UINTN)Cookie);
        break;
      }

      default:
        DEBUG ((DEBUG_WARN, "%a: unhandled RPC func %x\n", __func__, Args.Arg0));
        break;
    }

    Args.Arg0 = OPTEE_SMC_CALL_RETURN_FROM_RPC;
  }

  return (UINT32)Args.Arg0;
}

/**
  EFI_MM_COMMUNICATION2_PROTOCOL.Communicate: hand the MM buffer to StMM.
**/
STATIC
EFI_STATUS
EFIAPI
MmCommunicationOpteeCommunicate (
  IN CONST EFI_MM_COMMUNICATION2_PROTOCOL  *This,
  IN OUT VOID                              *CommBufferPhysical,
  IN OUT VOID                              *CommBufferVirtual,
  IN OUT UINTN                             *CommSize OPTIONAL
  )
{
  EFI_MM_COMMUNICATE_HEADER  *Header;
  OPTEE_MSG_ARG              *MsgArg;
  UINTN                      BufferSize;
  UINTN                      ArgSize;
  UINTN                      ShmComm;
  UINT32                     Rc;

  if ((CommBufferPhysical == NULL) || (CommBufferVirtual == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Post-ExitBootServices: the OP-TEE SHM window is not in the OS runtime
  // memory map and the flush path is gone -- refuse cleanly rather than
  // dereference physical addresses in a virtual world. The OS sees
  // EFI_UNSUPPORTED from Get/SetVariable; boot-time variables (BDS, Setup,
  // boot options) are unaffected. Lifting this needs the SHM window mapped
  // runtime + pointer conversion + an OS-side flush service.
  //
  if (mAtRuntime) {
    return EFI_UNSUPPORTED;
  }

  Header     = (EFI_MM_COMMUNICATE_HEADER *)CommBufferVirtual;
  BufferSize = OFFSET_OF (EFI_MM_COMMUNICATE_HEADER, Data) +
               (UINTN)Header->MessageLength;
  if ((CommSize != NULL) && (*CommSize != 0)) {
    BufferSize = *CommSize;
  }

  if (!mHaveSession) {
    return EFI_NOT_STARTED;
  }

  //
  // Per-call SHM layout: [invoke arg][comm buffer copy][RPC scratch...].
  //
  mShmNext = mShmBase;
  ArgSize  = SHM_ALIGN_UP (OPTEE_MSG_ARG_SIZE (2));
  MsgArg   = (OPTEE_MSG_ARG *)MmOpteeShmAlloc (ArgSize);
  ShmComm  = MmOpteeShmAlloc (BufferSize);
  if ((MsgArg == NULL) || (ShmComm == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: comm buffer (%lu B) exceeds OP-TEE SHM\n", __func__, (UINT64)BufferSize));
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem ((VOID *)ShmComm, (VOID *)CommBufferPhysical, BufferSize);

  ZeroMem (MsgArg, ArgSize);
  MsgArg->Cmd       = OPTEE_MSG_CMD_INVOKE_COMMAND;
  MsgArg->Func      = PTA_STMM_CMD_COMMUNICATE;
  MsgArg->Session   = mSession;
  MsgArg->NumParams = 2;
  //
  // [in/out] memref[0] = EFI MM communication buffer.
  //
  MsgArg->Params[0].Attr          = OPTEE_MSG_ATTR_TYPE_TMEM_INOUT;
  MsgArg->Params[0].U.TMem.BufPtr = (UINT64)ShmComm;
  MsgArg->Params[0].U.TMem.Size   = BufferSize;
  MsgArg->Params[0].U.TMem.ShmRef = (UINT64)ShmComm;
  //
  // [out] value[1].a = EFI return code from StMM.
  //
  MsgArg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;

  Rc = MmOpteeCallWithRpc ((UINTN)MsgArg);
  if ((Rc != 0) || (MsgArg->Ret != TEE_SUCCESS)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: StMM communicate failed smc=0x%x ret=0x%x\n",
      __func__,
      Rc,
      MsgArg->Ret
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // StMM edited the buffer in place; copy it back to the caller.
  //
  CopyMem ((VOID *)CommBufferPhysical, (VOID *)ShmComm, BufferSize);

  //
  // A successful SetVariable is the one path that changes the NV window --
  // tell the persistence engine (VarStoreSync.c). The Function field is
  // caller-owned and survives the round trip.
  //
  if (!EFI_ERROR ((EFI_STATUS)MsgArg->Params[1].U.Value.A) &&
      CompareGuid (&Header->HeaderGuid, &gEfiSmmVariableProtocolGuid) &&
      (Header->MessageLength >= SMM_VARIABLE_COMMUNICATE_HEADER_SIZE))
  {
    if (((SMM_VARIABLE_COMMUNICATE_HEADER *)Header->Data)->Function ==
        SMM_VARIABLE_FUNCTION_SET_VARIABLE)
    {
      VarStoreSyncMarkDirty ();
    }
  }

  return (EFI_STATUS)MsgArg->Params[1].U.Value.A;
}

STATIC EFI_MM_COMMUNICATION2_PROTOCOL  mMmCommunication2 = {
  MmCommunicationOpteeCommunicate
};

//
// Marker interface for gEfiSmmVariableProtocolGuid (see the install in the
// entry point). VariableSmmRuntimeDxe only checks it exists; the function
// pointers are deliberately NULL -- any future caller dereferencing them
// should fail loudly, because MM-side variable access from DXE goes through
// Communicate, never through this struct.
//
STATIC EFI_SMM_VARIABLE_PROTOCOL  mSmmVariableMarker;

/**
  ExitBootServices: no more MM communicates (see the guard in Communicate)
  and no more flushing -- runtime variable access is refused from here on.
**/
STATIC
VOID
EFIAPI
MmOpteeExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mAtRuntime = TRUE;
}

/**
  SetVirtualAddressMap: the protocol struct lives in this runtime image and
  the OS calls through it, so its function pointer must move to the virtual
  world with us. Everything the runtime path touches beyond it is a plain
  BOOLEAN.
**/
STATIC
VOID
EFIAPI
MmOpteeVirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gRT->ConvertPointer (0, (VOID **)&mMmCommunication2.Communicate);
}

/**
  Open the StMM pseudo-TA session through OUR RPC-aware loop.

  LOAD-BEARING: StMM runs its entire initialisation -- including the
  OpTeeRpmbFvb store reads that drive OP-TEE's REE-FS / RPMB RPCs -- inside
  stmm_complete_session(), which OP-TEE executes during OPEN_SESSION, NOT during
  the later invoke/Communicate. OpteeLib's OpteeOpenSession() drives OPEN_SESSION
  with OpteeCallWithArg(), whose RPC loop services only foreign interrupts and
  drops every OPTEE_RPC_CMD_* ("default: Do nothing"). That silently fails
  StMM's load-time storage, so StMM panics before Communicate ever runs and our
  ReeFs/RPMB servicer is never reached. We must therefore issue OPEN_SESSION
  ourselves and pump MmOpteeCallWithRpc(), so those bring-up RPCs reach
  MmOpteeRpcCmd() -> OpteeReeFsServiceCmd()/OpteeRpmbServiceCmd().

  @param[in]   Uuid      StMM pseudo-TA UUID.
  @param[out]  Session   Opened session id on success.

  @retval EFI_SUCCESS         Session opened.
  @retval EFI_OUT_OF_RESOURCES SHM too small for the arg.
  @retval EFI_NOT_FOUND       OP-TEE refused the session (StMM absent / panicked).
**/
STATIC
EFI_STATUS
MmOpteeOpenStmmSession (
  IN  CONST EFI_GUID  *Uuid,
  OUT UINT32          *Session
  )
{
  OPTEE_MSG_ARG  *MsgArg;
  UINT8          *Val;
  UINT32         D1;
  UINT16         D2;
  UINT16         D3;
  UINTN          ArgSize;
  UINT32         Rc;

  //
  // Reset the bump allocator; StMM's bring-up SHM_ALLOC RPCs allocate more
  // from the same window during the call below.
  //
  mShmNext = mShmBase;
  ArgSize  = SHM_ALIGN_UP (OPTEE_MSG_ARG_SIZE (2));
  MsgArg   = (OPTEE_MSG_ARG *)MmOpteeShmAlloc (ArgSize);
  if (MsgArg == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (MsgArg, ArgSize);
  MsgArg->Cmd       = OPTEE_MSG_CMD_OPEN_SESSION;
  MsgArg->NumParams = 2;

  //
  // Two meta params: [0] = TA UUID, [1] = client UUID (nil) + login method.
  // The UUID is packed big-endian (RFC4122) into value.a/value.b, byte-for-byte
  // as OpteeLib's EfiGuidToRfc4122Uuid does, so OP-TEE's UUID match succeeds.
  //
  MsgArg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META;
  Val                    = (UINT8 *)&MsgArg->Params[0].U.Value;
  D1                     = SwapBytes32 (Uuid->Data1);
  D2                     = SwapBytes16 (Uuid->Data2);
  D3                     = SwapBytes16 (Uuid->Data3);
  CopyMem (Val + 0, &D1, sizeof (D1));
  CopyMem (Val + 4, &D2, sizeof (D2));
  CopyMem (Val + 6, &D3, sizeof (D3));
  CopyMem (Val + 8, (VOID *)Uuid->Data4, sizeof (Uuid->Data4));

  MsgArg->Params[1].Attr      = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META;
  MsgArg->Params[1].U.Value.C = OPTEE_MSG_LOGIN_PUBLIC;

  Rc = MmOpteeCallWithRpc ((UINTN)MsgArg);
  if ((Rc != 0) || (MsgArg->Ret != TEE_SUCCESS)) {
    DEBUG ((
      DEBUG_ERROR,
      "REEFS: OPEN_SESSION failed smc=0x%x ret=0x%x origin=0x%x\n",
      Rc,
      MsgArg->Ret,
      MsgArg->RetOrigin
      ));
    return EFI_NOT_FOUND;
  }

  *Session = MsgArg->Session;
  return EFI_SUCCESS;
}

/**
  Driver entry: init OP-TEE, open the StMM session, publish MM_COMMUNICATION2.
**/
EFI_STATUS
EFIAPI
MmCommunicationOpteeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             Handle;
  EFI_EVENT              Event;
  STATIC CONST EFI_GUID  StmmUuid = PTA_STMM_UUID;

  if (!IsOpteePresent ()) {
    DEBUG ((DEBUG_WARN, "%a: OP-TEE not present; MM/variable store unavailable\n", __func__));
    return EFI_UNSUPPORTED;
  }

  Status = OpteeInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: OpteeInit failed - %r\n", __func__, Status));
    return Status;
  }

  Status = MmOpteeGetShm ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no OP-TEE shared memory - %r\n", __func__, Status));
    return Status;
  }

  //
  // Open via our own RPC-aware loop, NOT OpteeLib's OpteeOpenSession: StMM
  // runs its whole bring-up during OPEN_SESSION and OpteeLib drops RPCs (see
  // MmOpteeOpenStmmSession). The variable store is the FD NV window OP-TEE
  // mapped into the SP -- available with no device dependency, so there is
  // no ordering wait and no storage RPC traffic during bring-up either.
  //
  Status = MmOpteeOpenStmmSession (&StmmUuid, &mSession);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: open StMM session failed - %r (is CFG_STMM_PATH set in OP-TEE?)\n",
      __func__,
      Status
      ));
    return EFI_NOT_FOUND;
  }

  mHaveSession = TRUE;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEfiMmCommunication2ProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mMmCommunication2
                  );
  if (EFI_ERROR (Status)) {
    OpteeCloseSession (mSession);
    mHaveSession = FALSE;
    return Status;
  }

  //
  // Unblock VariableSmmRuntimeDxe. It arms protocol notifies on
  // gEfiSmmVariableProtocolGuid and gSmmVariableWriteGuid in the DXE
  // database and initializes nothing until they appear -- on x86 the
  // traditional VariableSmm installs them itself through boot services,
  // which a StandaloneMM image cannot do, so on this platform the MM
  // transport does it. The session open above ran StMM's ENTIRE bring-up
  // (FVB + FTW + variable driver on the mapped FD window), so both
  // services are genuinely ready. The interfaces are markers only:
  // VariableSmmRuntimeDxe locates them, never calls through them (all
  // variable traffic goes through Communicate).
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiSmmVariableProtocolGuid,
                  &mSmmVariableMarker,
                  &gSmmVariableWriteGuid,
                  NULL,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // Persistence engine for the FD NV window (VarStoreSync.c). Not fatal if
  // it cannot arm: variables still work for this boot, they just do not
  // survive it -- loudly, not silently.
  //
  Status = VarStoreSyncInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: variable persistence NOT armed - %r; "
      "variable changes will not survive this boot\n",
      __func__,
      Status
      ));
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  MmOpteeExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  MmOpteeVirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_INFO, "%a: StMM MM transport up (session 0x%x)\n", __func__, mSession));
  return EFI_SUCCESS;
}
