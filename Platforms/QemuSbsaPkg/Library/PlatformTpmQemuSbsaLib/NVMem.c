/** @file
  Tpm Nv Storage part of PlatformTpmLib to use TpmLib.

  Implements TPM NV storage backed by NOR flash via FVB protocol
  (gEfiSmmFirmwareVolumeBlockProtocolGuid).

  To see the plat_XXX interfaces in TPM reference library, see:
    - https://github.com/TrustedComputingGroup/TPM/tree/main/TPMCmd/Platform/src

  Copyright (c) 2025, Arm Limited. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/
#include <PiMm.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MmServicesTableLib.h>
#include <Library/PcdLib.h>
#include <Library/PlatformTpmLib.h>
#include <Protocol/FirmwareVolumeBlock.h>

//
// NV_MEMORY_SIZE must match the value in TpmProfile_Misc.h used by TpmLib.
//
#define NV_MEMORY_SIZE  16384

STATIC UINT8    *mTpmNvMemory;
STATIC BOOLEAN  mNvEnabled;
STATIC BOOLEAN  mNvNeedsManufacture;

STATIC EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *mTpmNvFvb;

/**
  Locate the FVB protocol instance for the TPM NV storage region.

  Iterates over all FVB handles and finds the one whose physical
  address matches PcdTpmNvStorageBase.

  @retval EFI_SUCCESS    FVB located and stored in mTpmNvFvb.
  @retval Others         FVB not found.
**/
STATIC
EFI_STATUS
LocateTpmNvFvb (
  VOID
  )
{
  EFI_STATUS                             Status;
  UINTN                                  BufferSize;
  EFI_HANDLE                             *HandleBuffer;
  UINTN                                  HandleCount;
  UINTN                                  Index;
  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL    *Fvb;
  EFI_PHYSICAL_ADDRESS                   FvbBaseAddress;
  UINT64                                 TpmNvBase;

  TpmNvBase  = FixedPcdGet64 (PcdTpmNvStorageBase);
  BufferSize = 0;

  Status = gMmst->MmLocateHandle (
                    ByProtocol,
                    &gEfiSmmFirmwareVolumeBlockProtocolGuid,
                    NULL,
                    &BufferSize,
                    NULL
                    );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, "%a: No FVB handles found. Status: %r\n", __func__, Status));
    return EFI_NOT_FOUND;
  }

  HandleBuffer = AllocatePool (BufferSize);
  if (HandleBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gMmst->MmLocateHandle (
                    ByProtocol,
                    &gEfiSmmFirmwareVolumeBlockProtocolGuid,
                    NULL,
                    &BufferSize,
                    HandleBuffer
                    );
  if (EFI_ERROR (Status)) {
    FreePool (HandleBuffer);
    return Status;
  }

  HandleCount = BufferSize / sizeof (EFI_HANDLE);

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gMmst->MmHandleProtocol (
                      HandleBuffer[Index],
                      &gEfiSmmFirmwareVolumeBlockProtocolGuid,
                      (VOID **)&Fvb
                      );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fvb->GetPhysicalAddress (Fvb, &FvbBaseAddress);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (FvbBaseAddress == TpmNvBase) {
      mTpmNvFvb = Fvb;
      FreePool (HandleBuffer);
      return EFI_SUCCESS;
    }
  }

  FreePool (HandleBuffer);
  DEBUG ((DEBUG_ERROR, "%a: FVB for TPM NV (base=0x%lx) not found\n", __func__, TpmNvBase));
  return EFI_NOT_FOUND;
}

/**
  Read TPM NV data from flash into local buffer via FVB.

  @retval EFI_SUCCESS   Data read successfully.
  @retval Others        Read failed.
**/
STATIC
EFI_STATUS
ReadNvFromFlash (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       NumBytes;

  NumBytes = NV_MEMORY_SIZE;

  Status = mTpmNvFvb->Read (
                         mTpmNvFvb,
                         0,       // LBA 0
                         0,       // Offset 0
                         &NumBytes,
                         mTpmNvMemory
                         );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: FVB Read failed: %r\n", __func__, Status));
  }

  return Status;
}

/**
  Write the entire TPM NV buffer to flash via FVB.
  Erases the block first, then writes the data.

  @retval EFI_SUCCESS   Data written successfully.
  @retval Others        Write failed.
**/
STATIC
EFI_STATUS
WriteNvToFlash (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       NumBytes;

  //
  // Erase the block before writing
  //
  Status = mTpmNvFvb->EraseBlocks (
                         mTpmNvFvb,
                         (EFI_LBA)0,
                         1,
                         EFI_LBA_LIST_TERMINATOR
                         );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: FVB EraseBlocks failed: %r\n", __func__, Status));
    return Status;
  }

  NumBytes = NV_MEMORY_SIZE;

  Status = mTpmNvFvb->Write (
                         mTpmNvFvb,
                         0,       // LBA 0
                         0,       // Offset 0
                         &NumBytes,
                         mTpmNvMemory
                         );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: FVB Write failed: %r\n", __func__, Status));
  }

  return Status;
}

/**
  _plat__NVNeedsManufacture()

  This function checks whether TPM's NV state needs to be manufactured.

  @return TRUE    TPM's NV storage should be manufactured
  @return FALSE   TPM's NV storage is already manufactured

**/
BOOLEAN
EFIAPI
PlatformTpmLibNVNeedsManufacture (
  VOID
  )
{
  return mNvNeedsManufacture;
}

/**
  _plat__NVEnable()

  Enable NV memory.

  Locates the FVB protocol for the TPM NV flash region,
  allocates a RAM buffer, and reads the NV state from flash.

  @param [in] PlatParameter   Platform parameter (unused)
  @param [in] ParamSize       Size of PlatParameter (unused)

  @return 0        if success
  @return < 0      if unrecoverable error

**/
INT32
EFIAPI
PlatformTpmLibNVEnable (
  IN VOID   *PlatParameter,
  IN UINTN  ParamSize
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  BOOLEAN     IsErased;

  if (mNvEnabled) {
    return 0;
  }

  //
  // Locate the FVB for TPM NV storage
  //
  Status = LocateTpmNvFvb ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to locate TPM NV FVB: %r\n", __func__, Status));
    return -1;
  }

  //
  // Allocate RAM buffer for TPM NV
  //
  mTpmNvMemory = AllocateZeroPool (NV_MEMORY_SIZE);
  if (mTpmNvMemory == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to allocate NV memory\n", __func__));
    return -1;
  }

  //
  // Read NV data from flash
  //
  Status = ReadNvFromFlash ();
  if (EFI_ERROR (Status)) {
    FreePool (mTpmNvMemory);
    mTpmNvMemory = NULL;
    return -1;
  }

  //
  // Check if flash is in erased state (all 0xFF) - needs manufacture
  //
  IsErased = TRUE;
  for (Index = 0; Index < NV_MEMORY_SIZE; Index++) {
    if (mTpmNvMemory[Index] != 0xFF) {
      IsErased = FALSE;
      break;
    }
  }

  mNvNeedsManufacture = IsErased;

  //
  // If erased, zero-fill the buffer as TpmLib expects zeroed NV on manufacture
  //
  if (IsErased) {
    ZeroMem (mTpmNvMemory, NV_MEMORY_SIZE);
  }

  mNvEnabled = TRUE;
  return 0;
}

/**
  _plat__NVDisable()

  Disable NV memory.

**/
VOID
EFIAPI
PlatformTpmLibNVDisable (
  VOID
  )
{
  mNvEnabled = FALSE;
  return;
}

/**
  _plat__GetNvReadyState()

  Check if NV is available.

  @return    0               NV is available
  @return    1               NV is not available due to write failure
  @return    2               NV is not available due to rate limit

**/
INT32
EFIAPI
PlatformTpmLibGetNvReadyState (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "%a: here %d %d\n", __func__, __LINE__, mNvEnabled));
  if (mNvEnabled) {
    return 0;
  }

  return 1;
}

/**
  _plat__NvMemoryRead()

  Read a chunk of NV memory.

  @param [in] StartOffset   Read start offset
  @param [in] Size          Size to read
  @param [out] Data         Data buffer

  @return 1 Success to read
  @return 0 Failed to read

**/
INT32
EFIAPI
PlatformTpmLibNvMemoryRead (
  IN  UINT32  StartOffset,
  IN  UINT32  Size,
  OUT VOID    *Data
  )
{
  if (!mNvEnabled || (mTpmNvMemory == NULL)) {
    return 0;
  }

  if ((StartOffset + Size) > NV_MEMORY_SIZE) {
    return 0;
  }

  CopyMem (Data, mTpmNvMemory + StartOffset, Size);
  return 1;
}

/**
  _plat__NvGetChangedStatus()

  This function checks to see if the NV is different from the test value.

  @param [in] StartOffset Start offset to compare
  @param [in] Size        Size for compare
  @param [in] Data        Data to be compared

  @return NV_HAS_CHANGED(1)       the NV location is different from the test value
  @return NV_IS_SAME(0)           the NV location is the same as the test value
  @return NV_INVALID_LOCATION(-1) the NV location is invalid
**/
INT32
EFIAPI
PlatformTpmLibNvGetChangedStatus (
  IN  UINT32  StartOffset,
  IN  UINT32  Size,
  IN  VOID    *Data
  )
{
  if (!mNvEnabled || (mTpmNvMemory == NULL)) {
    return -1;
  }

  if ((StartOffset + Size) > NV_MEMORY_SIZE) {
    return -1;
  }

  if (CompareMem (mTpmNvMemory + StartOffset, Data, Size) == 0) {
    return 0;
  }

  return 1;
}

/**
  _plat__NvMemoryWrite()

  Write to the NV memory buffer (in RAM).

  @param [in] StartOffset   Start Offset to write
  @param [in] Size          Size to write
  @param [in] Data          Data

  @return    0               Failed to write
  @return    1               Success to write

**/
INT32
EFIAPI
PlatformTpmLibNvMemoryWrite (
  IN  UINT32  StartOffset,
  IN  UINT32  Size,
  IN  VOID    *Data
  )
{
  DEBUG ((DEBUG_INFO, "%a: here %d\n", __func__, __LINE__));
  if (!mNvEnabled || (mTpmNvMemory == NULL)) {
    return 0;
  }
  DEBUG ((DEBUG_INFO, "%a: here %d\n", __func__, __LINE__));

  if ((StartOffset + Size) > NV_MEMORY_SIZE) {
    return 0;
  }
  DEBUG ((DEBUG_INFO, "%a: here %d\n", __func__, __LINE__));

  CopyMem (mTpmNvMemory + StartOffset, Data, Size);
  DEBUG ((DEBUG_INFO, "%a: here %d\n", __func__, __LINE__));
  return 1;
}

/**
  _plat__NvMemoryClear()

  Set a range of NV memory bytes to the erase value (0x00).

  @param [in] StartOffset   Start offset to clear
  @param [in] Size          Size to clear

  @return 0 Failed to clear
  @return 1 Success

**/
INT32
EFIAPI
PlatformTpmLibNvMemoryClear (
  IN  UINT32  StartOffset,
  IN  UINT32  Size
  )
{
  if (!mNvEnabled || (mTpmNvMemory == NULL)) {
    return 0;
  }

  if ((StartOffset + Size) > NV_MEMORY_SIZE) {
    return 0;
  }

  ZeroMem (mTpmNvMemory + StartOffset, Size);
  return 1;
}

/**
  _plat__NvMemoryMove()

  Move a chunk of NV memory from source to destination.
  Handles overlapping regions.

  @param [in] SourceOffset  Source offset to move
  @param [in] DestOffset    Destination offset to move
  @param [in] Size          Size to be moved

  @return 0 Failed to move
  @return 1 Success

**/
INT32
EFIAPI
PlatformTpmLibNvMemoryMove (
  IN   UINT32  SourceOffset,
  IN   UINT32  DestOffset,
  IN   UINT32  Size
  )
{
  if (!mNvEnabled || (mTpmNvMemory == NULL)) {
    return 0;
  }

  if (((SourceOffset + Size) > NV_MEMORY_SIZE) ||
      ((DestOffset + Size) > NV_MEMORY_SIZE))
  {
    return 0;
  }

  //
  // CopyMem handles overlapping regions correctly.
  //
  CopyMem (mTpmNvMemory + DestOffset, mTpmNvMemory + SourceOffset, Size);
  return 1;
}

/**
  _plat__NvCommit()

  Write the local copy of NV to persistent flash storage via FVB.

   @return 0       NV write success
   @return non-0   NV write fail

**/
INT32
EFIAPI
PlatformTpmLibNvCommit (
  VOID
  )
{
  EFI_STATUS  Status;

  if (!mNvEnabled || (mTpmNvMemory == NULL) || (mTpmNvFvb == NULL)) {
    return -1;
  }

  Status = WriteNvToFlash ();
  if (EFI_ERROR (Status)) {
    return -1;
  }

  return 0;
}

/**
  _plat__SetNvAvail()

   Set the current NV state to available.
   This function is for testing purpose only.

**/
VOID
EFIAPI
PlatformTpmLibSetNvAvail (
  VOID
  )
{
  return;
}

/**
  _plat__ClearNvAvail()

  Set the current NV state to unavailable.
  This function is for testing purpose only.

**/
VOID
EFIAPI
PlatformTpmLibClearNvAvail (
  VOID
  )
{
  return;
}

VOID
EFIAPI
PlatformTpmLibTearDown (
  VOID
)
{
  if (mTpmNvMemory != NULL) {
    FreePool (mTpmNvMemory);
    mTpmNvMemory = NULL;
  }

  mNvEnabled = FALSE;
  mTpmNvFvb  = NULL;
  return;
}
