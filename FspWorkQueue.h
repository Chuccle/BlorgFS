#pragma once

NTSTATUS FsdPostRequest(IN PIRP Irp, IN PIO_STACK_LOCATION IrpSp);

void PrePostIrp(IN PVOID Context, IN PIRP Irp);

void OplockComplete(PVOID Context, PIRP Irp);

NTSTATUS CreateWorkQueue(void);

void DestroyWorkQueue(void);