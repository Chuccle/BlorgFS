#pragma once

NTSTATUS BlorgCreate(PDEVICE_OBJECT pDeviceObject, PIRP pIrp);
NTSTATUS BlorgFCBCreate(FCB** ppFcb, PDCB parentDcb, CSHORT nodeType, PCUNICODE_STRING name);
NTSTATUS BlorgDCBCreate(DCB** ppDcb, PDCB parentDcb, CSHORT nodeType, PCUNICODE_STRING name);