/* FolioOS — "outro sistema" de mentira para o laboratório QEMU (fase 2, dual boot).
 * Um programa EFI mínimo que faz o papel do Windows Boot Manager: pinta a tela de azul e escreve
 * "FAKE WINDOWS STARTED" no console (que o OVMF também manda para a porta serial). O laboratório o coloca
 * na partição EFI de um disco "com Windows" e confere que o seletor de boot do FolioOS o inicia. */
#include "../src/platform/uefi/efi.h"

static EFI_GUID gop_guid = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    (void)image;
    static CHAR16 msg[] = { 'F','A','K','E',' ','W','I','N','D','O','W','S',' ','S','T','A','R','T','E','D','\r','\n',0 };
    st->ConOut->OutputString(st->ConOut, msg);
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (!EFI_ERROR(st->BootServices->LocateProtocol(&gop_guid, 0, (void **)&gop)) && gop && gop->Mode && gop->Mode->Info) {
        UINT32 blue = 0x000050C8;                     /* azul (B=0xC8, G=0x50) */
        gop->Blt(gop, &blue, EfiBltVideoFill, 0, 0, 0, 0, gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution, 0);
    }
    for (;;) st->BootServices->Stall(1000000);
}
