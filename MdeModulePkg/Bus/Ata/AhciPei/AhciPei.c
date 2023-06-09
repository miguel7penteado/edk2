/** @file
  The AhciPei driver is used to manage ATA hard disk device working under AHCI
  mode at PEI phase.

  Copyright (c) 2019, Intel Corporation. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "AhciPei.h"
#include <Ppi/PciDevice.h>
#include <Library/DevicePathLib.h>
#include <IndustryStandard/Pci.h>

EFI_PEI_PPI_DESCRIPTOR  mAhciAtaPassThruPpiListTemplate = {
  (EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST),
  &gEdkiiPeiAtaPassThruPpiGuid,
  NULL
};

EFI_PEI_PPI_DESCRIPTOR  mAhciBlkIoPpiListTemplate = {
  (EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST),
  &gEfiPeiVirtualBlockIoPpiGuid,
  NULL
};

EFI_PEI_PPI_DESCRIPTOR  mAhciBlkIo2PpiListTemplate = {
  (EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST),
  &gEfiPeiVirtualBlockIo2PpiGuid,
  NULL
};

EFI_PEI_PPI_DESCRIPTOR  mAhciStorageSecurityPpiListTemplate = {
  (EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST),
  &gEdkiiPeiStorageSecurityCommandPpiGuid,
  NULL
};

EFI_PEI_NOTIFY_DESCRIPTOR  mAhciEndOfPeiNotifyListTemplate = {
  (EFI_PEI_PPI_DESCRIPTOR_NOTIFY_CALLBACK | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST),
  &gEfiEndOfPeiSignalPpiGuid,
  AhciPeimEndOfPei
};

EFI_PEI_NOTIFY_DESCRIPTOR  mAtaAhciHostControllerNotify = {
  (EFI_PEI_PPI_DESCRIPTOR_NOTIFY_CALLBACK | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST),
  &gEdkiiPeiAtaAhciHostControllerPpiGuid,
  AtaAhciHostControllerPpiInstallationCallback
};

EFI_PEI_NOTIFY_DESCRIPTOR  mPciDevicePpiNotify = {
  (EFI_PEI_PPI_DESCRIPTOR_NOTIFY_CALLBACK | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST),
  &gEdkiiPeiPciDevicePpiGuid,
  AtaAhciPciDevicePpiInstallationCallback
};

/**
  Free the DMA resources allocated by an ATA AHCI controller.

  @param[in] Private    A pointer to the PEI_AHCI_CONTROLLER_PRIVATE_DATA data
                        structure.

**/
VOID
AhciFreeDmaResource (
  IN PEI_AHCI_CONTROLLER_PRIVATE_DATA  *Private
  )
{
  EFI_AHCI_REGISTERS  *AhciRegisters;

  ASSERT (Private != NULL);

  AhciRegisters = &Private->AhciRegisters;

  if (AhciRegisters->AhciRFisMap != NULL) {
    IoMmuFreeBuffer (
      EFI_SIZE_TO_PAGES (AhciRegisters->MaxRFisSize),
      AhciRegisters->AhciRFis,
      AhciRegisters->AhciRFisMap
      );
  }

  if (AhciRegisters->AhciCmdListMap != NULL) {
    IoMmuFreeBuffer (
      EFI_SIZE_TO_PAGES (AhciRegisters->MaxCmdListSize),
      AhciRegisters->AhciCmdList,
      AhciRegisters->AhciCmdListMap
      );
  }

  if (AhciRegisters->AhciCmdTableMap != NULL) {
    IoMmuFreeBuffer (
      EFI_SIZE_TO_PAGES (AhciRegisters->MaxCmdTableSize),
      AhciRegisters->AhciCmdTable,
      AhciRegisters->AhciCmdTableMap
      );
  }
}

/**
  One notified function to cleanup the allocated DMA buffers at EndOfPei.

  @param[in] PeiServices         Pointer to PEI Services Table.
  @param[in] NotifyDescriptor    Pointer to the descriptor for the Notification
                                 event that caused this function to execute.
  @param[in] Ppi                 Pointer to the PPI data associated with this function.

  @retval EFI_SUCCESS    The function completes successfully

**/
EFI_STATUS
EFIAPI
AhciPeimEndOfPei (
  IN EFI_PEI_SERVICES           **PeiServices,
  IN EFI_PEI_NOTIFY_DESCRIPTOR  *NotifyDescriptor,
  IN VOID                       *Ppi
  )
{
  PEI_AHCI_CONTROLLER_PRIVATE_DATA  *Private;

  Private = GET_AHCI_PEIM_HC_PRIVATE_DATA_FROM_THIS_NOTIFY (NotifyDescriptor);
  AhciFreeDmaResource (Private);

  return EFI_SUCCESS;
}

/**
  Initialize and install PrivateData PPIs.

  @param[in] MmioBase            MMIO base address of specific AHCI controller
  @param[in] DevicePath          A pointer to the EFI_DEVICE_PATH_PROTOCOL
                                 structure.
  @param[in] DevicePathLength    Length of the device path.

  @retval EFI_SUCCESS  AHCI controller initialized and PPIs installed
  @retval others       Failed to initialize AHCI controller
**/
EFI_STATUS
AtaAhciInitPrivateData (
  IN UINTN                     MmioBase,
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN UINTN                     DevicePathLength
  )
{
  EFI_STATUS                        Status;
  UINT32                            PortBitMap;
  UINT8                             NumberOfPorts;
  PEI_AHCI_CONTROLLER_PRIVATE_DATA  *Private;
  EFI_BOOT_MODE                     BootMode;

  DEBUG ((DEBUG_INFO, "Initializing private data for ATA\n"));

  //
  // Get the current boot mode.
  //
  Status = PeiServicesGetBootMode (&BootMode);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Fail to get the current boot mode.\n", __func__));
    return Status;
  }

  //
  // Check validity of the device path of the ATA AHCI controller.
  //
  Status = AhciIsHcDevicePathValid (DevicePath, DevicePathLength);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: The device path is invalid.\n",
      __func__
      ));
    return Status;
  }

  //
  // For S3 resume performance consideration, not all ports on an ATA AHCI
  // controller will be enumerated/initialized. The driver consumes the
  // content within S3StorageDeviceInitList LockBox to get the ports that
  // will be enumerated/initialized during S3 resume.
  //
  if (BootMode == BOOT_ON_S3_RESUME) {
    NumberOfPorts = AhciS3GetEumeratePorts (DevicePath, DevicePathLength, &PortBitMap);
    if (NumberOfPorts == 0) {
      return EFI_SUCCESS;
    }
  } else {
    PortBitMap = MAX_UINT32;
  }

  //
  // Memory allocation for controller private data.
  //
  Private = AllocateZeroPool (sizeof (PEI_AHCI_CONTROLLER_PRIVATE_DATA));
  if (Private == NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Fail to allocate private data.\n",
      __func__
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Initialize controller private data.
  //
  Private->Signature        = AHCI_PEI_CONTROLLER_PRIVATE_DATA_SIGNATURE;
  Private->MmioBase         = MmioBase;
  Private->DevicePathLength = DevicePathLength;
  Private->DevicePath       = DevicePath;
  Private->PortBitMap       = PortBitMap;
  InitializeListHead (&Private->DeviceList);

  Status = AhciModeInitialization (Private);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->AtaPassThruMode.Attributes = EFI_ATA_PASS_THRU_ATTRIBUTES_PHYSICAL |
                                        EFI_ATA_PASS_THRU_ATTRIBUTES_LOGICAL;
  Private->AtaPassThruMode.IoAlign      = sizeof (UINTN);
  Private->AtaPassThruPpi.Revision      = EDKII_PEI_ATA_PASS_THRU_PPI_REVISION;
  Private->AtaPassThruPpi.Mode          = &Private->AtaPassThruMode;
  Private->AtaPassThruPpi.PassThru      = AhciAtaPassThruPassThru;
  Private->AtaPassThruPpi.GetNextPort   = AhciAtaPassThruGetNextPort;
  Private->AtaPassThruPpi.GetNextDevice = AhciAtaPassThruGetNextDevice;
  Private->AtaPassThruPpi.GetDevicePath = AhciAtaPassThruGetDevicePath;
  CopyMem (
    &Private->AtaPassThruPpiList,
    &mAhciAtaPassThruPpiListTemplate,
    sizeof (EFI_PEI_PPI_DESCRIPTOR)
    );
  Private->AtaPassThruPpiList.Ppi = &Private->AtaPassThruPpi;
  PeiServicesInstallPpi (&Private->AtaPassThruPpiList);

  Private->BlkIoPpi.GetNumberOfBlockDevices = AhciBlockIoGetDeviceNo;
  Private->BlkIoPpi.GetBlockDeviceMediaInfo = AhciBlockIoGetMediaInfo;
  Private->BlkIoPpi.ReadBlocks              = AhciBlockIoReadBlocks;
  CopyMem (
    &Private->BlkIoPpiList,
    &mAhciBlkIoPpiListTemplate,
    sizeof (EFI_PEI_PPI_DESCRIPTOR)
    );
  Private->BlkIoPpiList.Ppi = &Private->BlkIoPpi;
  PeiServicesInstallPpi (&Private->BlkIoPpiList);

  Private->BlkIo2Ppi.Revision                = EFI_PEI_RECOVERY_BLOCK_IO2_PPI_REVISION;
  Private->BlkIo2Ppi.GetNumberOfBlockDevices = AhciBlockIoGetDeviceNo2;
  Private->BlkIo2Ppi.GetBlockDeviceMediaInfo = AhciBlockIoGetMediaInfo2;
  Private->BlkIo2Ppi.ReadBlocks              = AhciBlockIoReadBlocks2;
  CopyMem (
    &Private->BlkIo2PpiList,
    &mAhciBlkIo2PpiListTemplate,
    sizeof (EFI_PEI_PPI_DESCRIPTOR)
    );
  Private->BlkIo2PpiList.Ppi = &Private->BlkIo2Ppi;
  PeiServicesInstallPpi (&Private->BlkIo2PpiList);

  if (Private->TrustComputingDevices != 0) {
    DEBUG ((
      DEBUG_INFO,
      "%a: Security Security Command PPI will be produced.\n",
      __func__
      ));
    Private->StorageSecurityPpi.Revision           = EDKII_STORAGE_SECURITY_PPI_REVISION;
    Private->StorageSecurityPpi.GetNumberofDevices = AhciStorageSecurityGetDeviceNo;
    Private->StorageSecurityPpi.GetDevicePath      = AhciStorageSecurityGetDevicePath;
    Private->StorageSecurityPpi.ReceiveData        = AhciStorageSecurityReceiveData;
    Private->StorageSecurityPpi.SendData           = AhciStorageSecuritySendData;
    CopyMem (
      &Private->StorageSecurityPpiList,
      &mAhciStorageSecurityPpiListTemplate,
      sizeof (EFI_PEI_PPI_DESCRIPTOR)
      );
    Private->StorageSecurityPpiList.Ppi = &Private->StorageSecurityPpi;
    PeiServicesInstallPpi (&Private->StorageSecurityPpiList);
  }

  CopyMem (
    &Private->EndOfPeiNotifyList,
    &mAhciEndOfPeiNotifyListTemplate,
    sizeof (EFI_PEI_NOTIFY_DESCRIPTOR)
    );
  PeiServicesNotifyPpi (&Private->EndOfPeiNotifyList);

  return EFI_SUCCESS;
}

/**
  Initialize AHCI controller from EDKII_ATA_AHCI_HOST_CONTROLLER_PPI instance.

  @param[in] AhciHcPpi  Pointer to the AHCI Host Controller PPI instance.

  @retval EFI_SUCCESS   PPI successfully installed.
**/
EFI_STATUS
AtaAhciInitPrivateDataFromHostControllerPpi (
  IN EDKII_ATA_AHCI_HOST_CONTROLLER_PPI  *AhciHcPpi
  )
{
  UINT8                     Controller;
  UINTN                     MmioBase;
  UINTN                     DevicePathLength;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_STATUS                Status;

  Controller = 0;
  MmioBase   = 0;
  while (TRUE) {
    Status = AhciHcPpi->GetAhciHcMmioBar (
                          AhciHcPpi,
                          Controller,
                          &MmioBase
                          );
    //
    // When status is error, meant no controller is found.
    //
    if (EFI_ERROR (Status)) {
      break;
    }

    Status = AhciHcPpi->GetAhciHcDevicePath (
                          AhciHcPpi,
                          Controller,
                          &DevicePathLength,
                          &DevicePath
                          );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: Fail to allocate get the device path for Controller %d.\n",
        __func__,
        Controller
        ));
      return Status;
    }

    Status = AtaAhciInitPrivateData (MmioBase, DevicePath, DevicePathLength);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: Controller initialization fail for Controller %d with Status - %r.\n",
        __func__,
        Controller,
        Status
        ));
    } else {
      DEBUG ((
        DEBUG_INFO,
        "%a: Controller %d has been successfully initialized.\n",
        __func__,
        Controller
        ));
    }

    Controller++;
  }

  return EFI_SUCCESS;
}

/**
  Callback for EDKII_ATA_AHCI_HOST_CONTROLLER_PPI installation.

  @param[in] PeiServices         Pointer to PEI Services Table.
  @param[in] NotifyDescriptor    Pointer to the descriptor for the Notification
                                 event that caused this function to execute.
  @param[in] Ppi                 Pointer to the PPI data associated with this function.

  @retval EFI_SUCCESS            The function completes successfully
  @retval Others                 Cannot initialize AHCI controller from given EDKII_ATA_AHCI_HOST_CONTROLLER_PPI

**/
EFI_STATUS
EFIAPI
AtaAhciHostControllerPpiInstallationCallback (
  IN EFI_PEI_SERVICES           **PeiServices,
  IN EFI_PEI_NOTIFY_DESCRIPTOR  *NotifyDescriptor,
  IN VOID                       *Ppi
  )
{
  EDKII_ATA_AHCI_HOST_CONTROLLER_PPI  *AhciHcPpi;

  if (Ppi == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  AhciHcPpi = (EDKII_ATA_AHCI_HOST_CONTROLLER_PPI *)Ppi;

  return AtaAhciInitPrivateDataFromHostControllerPpi (AhciHcPpi);
}

/**
  Initialize AHCI controller from fiven PCI_DEVICE_PPI.

  @param[in] PciDevice  Pointer to the PCI Device PPI instance.

  @retval EFI_SUCCESS      The function completes successfully
  @retval Others           Cannot initialize AHCI controller for given device
**/
EFI_STATUS
AtaAhciInitPrivateDataFromPciDevice (
  EDKII_PCI_DEVICE_PPI  *PciDevice
  )
{
  EFI_STATUS                Status;
  PCI_TYPE00                PciData;
  UINT32                    MmioBase;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  UINTN                     DevicePathLength;
  UINT64                    EnabledPciAttributes;

  //
  // Now further check the PCI header: Base Class (offset 0x0B) and
  // Sub Class (offset 0x0A). This controller should be an SATA controller
  //
  Status = PciDevice->PciIo.Pci.Read (
                                  &PciDevice->PciIo,
                                  EfiPciIoWidthUint8,
                                  PCI_CLASSCODE_OFFSET,
                                  sizeof (PciData.Hdr.ClassCode),
                                  PciData.Hdr.ClassCode
                                  );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  if (!IS_PCI_IDE (&PciData) && !IS_PCI_SATADPA (&PciData)) {
    return EFI_UNSUPPORTED;
  }

  Status = PciDevice->PciIo.Attributes (
                              &PciDevice->PciIo,
                              EfiPciIoAttributeOperationSupported,
                              0,
                              &EnabledPciAttributes
                              );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  } else {
    EnabledPciAttributes &= (UINT64)EFI_PCI_DEVICE_ENABLE;
    Status                = PciDevice->PciIo.Attributes (
                                               &PciDevice->PciIo,
                                               EfiPciIoAttributeOperationEnable,
                                               EnabledPciAttributes,
                                               NULL
                                               );
    if (EFI_ERROR (Status)) {
      return EFI_UNSUPPORTED;
    }
  }

  Status = PciDevice->PciIo.Pci.Read (
                                  &PciDevice->PciIo,
                                  EfiPciIoWidthUint32,
                                  0x24,
                                  1,
                                  &MmioBase
                                  );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  MmioBase &= 0xFFFFFFF0;

  DevicePathLength = GetDevicePathSize (PciDevice->DevicePath);
  DevicePath       = PciDevice->DevicePath;

  Status = AtaAhciInitPrivateData (MmioBase, DevicePath, DevicePathLength);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "%a: Failed to init controller, with Status - %r\n",
      __func__,
      Status
      ));
  }

  return EFI_SUCCESS;
}

/**
  Callback for EDKII_PCI_DEVICE_PPI installation.

  @param[in] PeiServices         Pointer to PEI Services Table.
  @param[in] NotifyDescriptor    Pointer to the descriptor for the Notification
                                 event that caused this function to execute.
  @param[in] Ppi                 Pointer to the PPI data associated with this function.

  @retval EFI_SUCCESS            The function completes successfully
  @retval Others                 Cannot initialize AHCI controller from given PCI_DEVICE_PPI

**/
EFI_STATUS
EFIAPI
AtaAhciPciDevicePpiInstallationCallback (
  IN EFI_PEI_SERVICES           **PeiServices,
  IN EFI_PEI_NOTIFY_DESCRIPTOR  *NotifyDescriptor,
  IN VOID                       *Ppi
  )
{
  EDKII_PCI_DEVICE_PPI  *PciDevice;

  PciDevice = (EDKII_PCI_DEVICE_PPI *)Ppi;

  return AtaAhciInitPrivateDataFromPciDevice (PciDevice);
}

/**
  Entry point of the PEIM.

  @param[in] FileHandle     Handle of the file being invoked.
  @param[in] PeiServices    Describes the list of possible PEI Services.

  @retval EFI_SUCCESS    PPI successfully installed.

**/
EFI_STATUS
EFIAPI
AtaAhciPeimEntry (
  IN EFI_PEI_FILE_HANDLE     FileHandle,
  IN CONST EFI_PEI_SERVICES  **PeiServices
  )
{
  DEBUG ((DEBUG_INFO, "%a: Enters.\n", __func__));

  PeiServicesNotifyPpi (&mAtaAhciHostControllerNotify);

  PeiServicesNotifyPpi (&mPciDevicePpiNotify);

  return EFI_SUCCESS;
}
