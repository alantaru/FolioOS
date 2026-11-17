/* FolioOS — instala o boot por BIOS num pendrive (ou imagem) FAT32 já com o FolioOS copiado.
 *
 *   biosboot.exe <imagem.img>            grava numa imagem de disco (MBR + FAT32, como as do mkimg)
 *   biosboot.exe E:                      grava no pendrive E: (precisa de administrador)
 *   biosboot.exe E: --check              só confere, não grava nada
 *
 * O que muda, e só isso:
 *   - setor de boot do volume FAT32 (e a cópia de reserva no setor 6): bytes 0..2 (salto) e 90..509
 *     (código). O BPB (bytes 3..89), que descreve o sistema de arquivos, fica exatamente como estava;
 *   - se o disco tiver tabela de partições: os 440 primeiros bytes do MBR (código). A assinatura do
 *     disco e a tabela de partições (bytes 440..511) ficam como estavam.
 * Nenhum arquivo é criado, movido, apagado ou renomeado; o pendrive continua um FAT32 comum, que o
 * Windows, o macOS e o Linux leem normalmente, e o boot por UEFI continua funcionando igual.
 * Antes de gravar, confere que o volume é FAT32 com setores de 512 bytes e que FOLIO\BIOS.SYS existe. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8; typedef uint32_t u32;
static u8 mbr_code[440], vbr_code[512];

static bool load(const char *dir, const char *name, u8 *buf, size_t n) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "não achei %s (rode o build.ps1)\n", p); return false; }
    size_t got = fread(buf, 1, n, f); fclose(f);
    if (got != n) { fprintf(stderr, "%s: tamanho errado (%zu bytes, esperado %zu)\n", p, got, n); return false; }
    return true;
}
static bool fat32_bpb(const u8 *s) {
    return s[510] == 0x55 && s[511] == 0xAA && s[11] == 0 && s[12] == 2 && s[13] && s[16] &&
           s[17] == 0 && s[18] == 0 && s[22] == 0 && s[23] == 0 && (s[36] | s[37] | s[38] | s[39]) && !memcmp(s + 82, "FAT32   ", 8);
}
static void patch_vbr(u8 *s) { memcpy(s, vbr_code, 3); memcpy(s + 90, vbr_code + 90, 420); }

/* ------------------------------------------------------------ disco abstrato: imagem ou dispositivo */
typedef struct { HANDLE h; bool dev; } Disk;
static bool rd(Disk *d, unsigned long long lba, u8 *buf) {
    LARGE_INTEGER o; o.QuadPart = (LONGLONG)(lba * 512);
    DWORD n = 0;
    return SetFilePointerEx(d->h, o, 0, FILE_BEGIN) && ReadFile(d->h, buf, 512, &n, 0) && n == 512;
}
static bool wr(Disk *d, unsigned long long lba, const u8 *buf) {
    LARGE_INTEGER o; o.QuadPart = (LONGLONG)(lba * 512);
    DWORD n = 0;
    return SetFilePointerEx(d->h, o, 0, FILE_BEGIN) && WriteFile(d->h, buf, 512, &n, 0) && n == 512;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { fprintf(stderr, "uso: biosboot <imagem.img | E:> [--check]\n"); return 2; }
    bool check = argc > 2 && !strcmp(argv[2], "--check");
    char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH); char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
    char bdir[MAX_PATH + 8]; snprintf(bdir, sizeof bdir, "%s\\bios", exe);
    if (!load(bdir, "mbr.bin", mbr_code, 440) || !load(bdir, "vbr.bin", vbr_code, 512)) return 1;
    const char *target = argv[1];
    bool is_drive = strlen(target) == 2 && target[1] == ':';
    Disk vol = { INVALID_HANDLE_VALUE, false }, disk = { INVALID_HANDLE_VALUE, false };
    unsigned long long part = 0;
    u8 s[512], m[512];

    if (!is_drive) {
        vol.h = CreateFileA(target, GENERIC_READ | (check ? 0 : GENERIC_WRITE), FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (vol.h == INVALID_HANDLE_VALUE) { fprintf(stderr, "não abriu %s\n", target); return 1; }
        disk = vol;
        if (!rd(&disk, 0, m)) { fprintf(stderr, "leitura falhou\n"); return 1; }
        if (!fat32_bpb(m)) {
            if (m[510] != 0x55 || m[511] != 0xAA) { fprintf(stderr, "%s: sem MBR nem FAT32\n", target); return 1; }
            for (int i = 0; i < 4 && !part; i++) { u8 t = m[446 + i * 16 + 4]; if (t == 0x0B || t == 0x0C || t == 0xEF) part = *(u32 *)(m + 446 + i * 16 + 8); }
            if (!part) { fprintf(stderr, "%s: nenhuma partição FAT32\n", target); return 1; }
        }
    } else {
        char vp[16]; snprintf(vp, sizeof vp, "\\\\.\\%c:", target[0]);
        char root[8]; snprintf(root, sizeof root, "%c:\\", target[0]);
        char fs[32] = ""; GetVolumeInformationA(root, 0, 0, 0, 0, 0, fs, sizeof fs);
        if (strcmp(fs, "FAT32")) { fprintf(stderr, "%s não é FAT32 (é %s)\n", target, fs[0] ? fs : "?"); return 1; }
        UINT type = GetDriveTypeA(root);
        if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) { fprintf(stderr, "%s não é um disco local\n", target); return 1; }
        char sys[32]; snprintf(sys, sizeof sys, "%c:\\FOLIO\\BIOS.SYS", target[0]);
        if (GetFileAttributesA(sys) == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "%s não existe: copie o FolioOS para o pendrive antes\n", sys); return 1; }
        vol.h = CreateFileA(vp, GENERIC_READ | (check ? 0 : GENERIC_WRITE), FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        if (vol.h == INVALID_HANDLE_VALUE) { fprintf(stderr, "não abriu %s (rode como administrador)\n", vp); return 1; }
        vol.dev = true;
        VOLUME_DISK_EXTENTS ext; DWORD got;
        if (!DeviceIoControl(vol.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0, 0, &ext, sizeof ext, &got, 0) || ext.NumberOfDiskExtents != 1) {
            fprintf(stderr, "não deu para saber em que disco está %s\n", target); return 1;
        }
        part = (unsigned long long)ext.Extents[0].StartingOffset.QuadPart / 512;
        if (part) {
            char dp[32]; snprintf(dp, sizeof dp, "\\\\.\\PhysicalDrive%lu", ext.Extents[0].DiskNumber);
            disk.h = CreateFileA(dp, GENERIC_READ | (check ? 0 : GENERIC_WRITE), FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            if (disk.h == INVALID_HANDLE_VALUE) { fprintf(stderr, "não abriu %s (rode como administrador)\n", dp); return 1; }
            disk.dev = true;
        }
        if (!check) {
            /* o Windows deixa gravar os setores de boot de um volume montado; travar é só precaução
             * (falha se algo estiver aberto no pendrive, por exemplo este programa rodando dele) */
            DWORD x;
            if (!DeviceIoControl(vol.h, FSCTL_LOCK_VOLUME, 0, 0, 0, 0, &x, 0)) printf("(pendrive em uso: gravando só os setores de boot, o que o Windows permite)\n");
        }
    }
    /* setor de boot do volume (relativo ao volume quando é um dispositivo) */
    unsigned long long vbase = vol.dev ? 0 : part;
    if (!rd(&vol, vbase, s)) { fprintf(stderr, "leitura do setor de boot falhou\n"); return 1; }
    if (!fat32_bpb(s)) { fprintf(stderr, "o volume não é FAT32 com setores de 512 bytes\n"); return 1; }
    if (part && !vol.dev && *(u32 *)(s + 28) != part) fprintf(stderr, "aviso: o BPB diz que a partição começa em %u, a tabela diz %llu\n", *(u32 *)(s + 28), part);
    bool vbr_done = s[90] == vbr_code[90] && !memcmp(s + 90, vbr_code + 90, 420);
    printf("volume FAT32 em LBA %llu: setor de boot %s\n", part, vbr_done ? "já tem o boot do FolioOS" : "receberá o boot do FolioOS");
    if (part) {
        if (!rd(&disk, 0, m)) { fprintf(stderr, "leitura do MBR falhou\n"); return 1; }
        printf("tabela de partições: MBR %s\n", !memcmp(m, mbr_code, 440) ? "já tem o boot do FolioOS" : "receberá o boot do FolioOS (a tabela fica intacta)");
    } else printf("pendrive sem tabela de partições: só o setor de boot do volume\n");
    if (check) return 0;

    patch_vbr(s);
    if (!wr(&vol, vbase, s)) { fprintf(stderr, "gravação do setor de boot falhou\n"); return 1; }
    u8 b6[512];
    if (rd(&vol, vbase + 6, b6) && fat32_bpb(b6)) { patch_vbr(b6); wr(&vol, vbase + 6, b6); }    /* cópia de reserva */
    if (part) {
        memcpy(m, mbr_code, 440);
        if (!wr(&disk, 0, m)) { fprintf(stderr, "gravação do MBR falhou\n"); return 1; }
    }
    if (vol.dev) { DWORD x; DeviceIoControl(vol.h, FSCTL_UNLOCK_VOLUME, 0, 0, 0, 0, &x, 0); }
    printf("pronto: o pendrive agora também dá boot em PCs só com BIOS (Legacy/CSM)\n");
    return 0;
}
