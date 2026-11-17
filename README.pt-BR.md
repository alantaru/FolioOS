<div align="center">

# FolioOS

### Uma máquina de escrever sem distrações que dá boot direto de um pen-drive

**Sem Windows. Sem Linux. Sem internet. Sem notificações. Só você e o texto.**

[English](README.md) · [**Baixar**](https://github.com/alantaru/FolioOS/releases/latest) ·
[Traduzir](docs/TRANSLATING.md) · [Contribuir](CONTRIBUTING.md)

<img src="docs/screenshots/hero.png" alt="Editor do FolioOS" width="860">

</div>

O FolioOS transforma quase qualquer PC ou notebook x86 numa máquina de escrever digital
dedicada: espete o pen-drive, dê boot e escreva. Seus textos ficam em arquivos Markdown
comuns, num pen-drive FAT32 que abre em qualquer computador.

## Destaques

- **Foco imediato:** liga em segundos no último documento, exatamente onde você parou.
- **Notebook antigo vira ferramenta de escritor:** roda em PCs UEFI de 64 e 32 bits, sem
  instalar nada no disco interno.
- **Seus arquivos para sempre:** `.md` em FAT32, sem banco de dados, sem nuvem.
- **Pequeno e auditável:** um binário de ~600 KB em C freestanding, sem libc nem kernel.
- **Universal:** 15 idiomas e 14 teclados, todos em arquivos de texto que qualquer pessoa pode acrescentar.

Escrita com Markdown ao vivo, modo foco, máquina de escrever, sprints e metas, modo leitura com
rolagem automática, exportação para **DOCX, ODT, EPUB, HTML, RTF e TXT**, versões automáticas,
20 temas e editor de temas, fontes TrueType até 4K, abas opcionais e player de MP3.

## Tour rápido

| | |
|---|---|
| <img src="docs/screenshots/focus-mode.png" alt="Modo foco"> **Modo foco (F2):** só o parágrafo atual em destaque. | <img src="docs/screenshots/reading-mode.png" alt="Modo leitura"> **Leitura (Ctrl+P):** o texto diagramado como livro, com rolagem automática. |
| <img src="docs/screenshots/export.png" alt="Exportar"> **Exportar (Ctrl+Shift+S):** DOCX, ODT, EPUB, HTML, RTF, TXT. | <img src="docs/screenshots/themes.png" alt="Temas"> **Temas (F12):** 20 prontos e editor para criar os seus. |
| <img src="docs/screenshots/command-palette.png" alt="Paleta de comandos"> **Paleta (Ctrl+K):** qualquer comando digitando poucas letras. | <img src="docs/screenshots/tabs.png" alt="Abas"> **Abas (opcional):** vários documentos, que voltam após reiniciar. |

O tour completo, com todas as telas, está no [README em inglês](README.md#a-tour-of-folioos).

## Instalar em 3 minutos

1. **Baixe** o `FolioOS.zip` da [última versão](https://github.com/alantaru/FolioOS/releases/latest).
2. **Descompacte na raiz de um pen-drive FAT32** (até 32 GB quase sempre já é FAT32; o que já
   estiver no pen-drive é mantido).
3. **Dê boot pelo pen-drive:** menu de boot (F12 Dell/Lenovo · F9 HP · F8 Asus · Esc/F11) e
   escolha a opção **UEFI**. Se não aparecer, desative o *Secure Boot*.

Pen-drives preparados por versões anteriores continuam com as pastas em português (`TEXTOS`,
`MUSICA`…), que seguem funcionando, e com a interface em português.

## Compilar

Windows 10/11 com `gcc` do [MinGW-w64](https://winlibs.com) no PATH:

```powershell
.\build.ps1                       # compila UEFI x64 + IA32, simulador, pacotes e roda os testes
.\flash_usb.ps1 -DriveLetter E:   # instala/atualiza um pen-drive FAT32 sem apagar arquivos
```

## Licença

Software livre sob a [GNU GPL v3.0](LICENSE). Fontes sob a SIL Open Font License; minimp3 em CC0.
