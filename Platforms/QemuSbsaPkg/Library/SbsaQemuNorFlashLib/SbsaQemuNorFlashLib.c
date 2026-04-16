/** @file

 Copyright (c) 2019, Linaro Ltd. All rights reserved
 Copyright (c) Microsoft Corporation.

 SPDX-License-Identifier: BSD-2-Clause-Patent

 **/

#include <Base.h>
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/VirtNorFlashPlatformLib.h>

#define QEMU_NOR_BLOCK_SIZE  SIZE_256KB

EFI_STATUS
VirtNorFlashPlatformInitialization (
  VOID
  )
{
  return EFI_SUCCESS;
}

VIRT_NOR_FLASH_DESCRIPTION  mNorFlashDevices[] =
{
  {
    FixedPcdGet64 (PcdFlashNvStorageBase),
    FixedPcdGet64 (PcdFlashNvStorageBase),
    FixedPcdGet32 (PcdFlashNvStorageSize),
    QEMU_NOR_BLOCK_SIZE
  },
  {
    FixedPcdGet64 (PcdTpmNvStorageBase),
    FixedPcdGet64 (PcdTpmNvStorageBase),
    FixedPcdGet32 (PcdTpmNvStorageSize),
    QEMU_NOR_BLOCK_SIZE
  }
};

EFI_STATUS
VirtNorFlashPlatformGetDevices (
  OUT VIRT_NOR_FLASH_DESCRIPTION  **NorFlashDescriptions,
  OUT UINT32                      *Count
  )
{
  *NorFlashDescriptions = mNorFlashDevices;
  *Count                = ARRAY_SIZE (mNorFlashDevices);
  return EFI_SUCCESS;
}
