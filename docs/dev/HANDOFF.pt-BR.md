# FOLIO 4 — documento de handoff

**Versão:** 4.0 · **Data:** 12/11/2026
**Antecessor:** FOLIO-86 v3.0 (100% Assembly, preservado em `src/legacy/`)

---

## 1. O que é

Uma máquina de escrever digital *distraction-free* que dá boot direto de um pen-drive em PCs x86/x64, sem sistema operacional. A v4 reescreveu tudo em **C freestanding** (sem libc, sem runtime, sem bibliotecas de SO) e compila para:

| Alvo | Arquivo | Para quê |
|---|---|---|
| UEFI x86_64 | `build\efi\BOOTX64.EFI` (~430 KB) | praticamente todo PC/notebook desde ~2012 |
| UEFI IA32 | `build\efi\BOOTIA32.EFI` | tablets e netbooks Atom com firmware 32-bit |
| Windows | `build\folio_sim.exe` | simulador para desenvolvimento e testes (mesmo núcleo) |
| Windows | `build\efi_host.exe` | **executa o próprio BOOTX64.EFI** com firmware UEFI emulado |
| BIOS legado | `build\legacy\folio86.img` (`.\build.ps1 -Legacy`) | FOLIO-86 v3 antigo, para PCs sem UEFI |

### Por que a reescrita
A v3 usava fontes bitmap 10×16, 8 "slots" fixos (`MANUSCR1..8.MD`), e não tinha gerenciador de arquivos nem áudio. Fontes vetoriais, Markdown renderizado, exportação para DOCX/ODT/EPUB e decodificação de MP3 não são viáveis em Assembly à mão. A nova base em C mantém a filosofia "zero dependências de SO".

## 2. Funcionalidades

- **Gráficos modernos:** resolução nativa do painel (lida do EDID via GOP), escala de interface automática (1080p, 1440p, 4K), anti-aliasing com cobertura exata por área, sombras suaves, animações.
- **Tipografia real:** rasterizador TrueType próprio (`font.c`) com glifos compostos, **kerning GPOS/kern**, posicionamento subpixel (1/4 px), negrito/itálico sintéticos quando faltam. Há 5 famílias OFL em `FOLIO\FONTES` (Spectral, Fira Sans, IBM Plex Mono, Courier Prime, Atkinson Hyperlegible). O usuário pode acrescentar qualquer `.ttf`. A Atkinson fica embutida no binário como garantia.
- **Sistema de janelas próprio** (`ui.c`): janelas arrastáveis e redimensionáveis, z-order, janelas modais com escurecimento, animação de abrir/fechar, tooltips, toasts, widgets (botões, sliders, toggles, controles segmentados, campos de texto).
- **Markdown:** estilo ao vivo no editor (o `#` fica pendurado na margem e os marcadores aparecem atenuados, com opção de ocultá-los fora do parágrafo atual) e **pré-visualização renderizada** (Ctrl+P) com títulos, citações, listas, tarefas, código, tabelas e barra de progresso de leitura.
- **Salvar em 7 formatos:** `.md`, `.txt`, `.html`, `.rtf`, `.docx`, `.odt`, `.epub` (capítulos por `#`). O ZIP é escrito à mão em modo "stored", com CRC32.
- **Gerenciador de arquivos completo** no abrir/salvar: todos os volumes, locais rápidos, breadcrumb, ordenação por nome/tamanho/data, criar pasta, renomear (F2), excluir (inclusive recursivo, com confirmação), confirmação de sobrescrita, escolha de formato.
- **Qualidade de vida para escritores:** barras que somem ao digitar e voltam com o mouse; ponteiro que se esconde; modo foco por frase, parágrafo ou linha (F2); máquina de escrever (F3); zen (F11); sprint cronometrado com sino (F4); meta de palavras com anel de progresso; estatísticas da sessão (F10); sumário clicável (F5); localizar/substituir com busca que ignora acentos e maiúsculas; paleta de comandos (Ctrl+K); tipografia inteligente (`--`→—, `...`→…, aspas curvas); continuação automática de listas e citações; Alt+↑/↓ move parágrafos; Ctrl+1..6 aplica títulos; modo Hemingway; cursor suave e piscante; rolagem suave; desfazer/refazer agrupado; **autosave** (padrão 20 s) com gravação atômica (`.tmp` + renomear); recuperação de rascunho sem nome; **a sessão é restaurada no boot** (arquivo e posição do cursor).
- **Proteção e continuidade:** versões automáticas em `FOLIO\VERSOES` (antes de sobrescrever, no máximo 1 a cada 10 min, as 30 últimas por documento), documentos recentes no menu, palavras escritas **hoje** somadas entre sessões e documentos.
- **Interface** (estudo em `docs\ESTUDO_INTERFACE.md`): barras em degradê que se fundem à folha, título centralizado derivado do texto, só modos de escrita na barra, menu agrupado, contagem da seleção, recuo de primeira linha opcional; cada linha é um parágrafo também na leitura e nas exportações.
- **20 temas:** Papiro, Neve, Obsidiana, E-Ink, Sépia, Oceano, Floresta, Meia-noite, Lavanda, Rosé, Grafite, Fósforo, Âmbar, Solar claro, Solar escuro, Alto contraste, Café, Duna, Névoa e Vinho.
- **Música:** os MP3 de `\MUSICA` (e subpastas) do pen-drive. Decodificação em streaming (minimp3, CC0), tags ID3v2, duração inclusive para VBR (Xing), busca na faixa, aleatório/repetir, volume, mini-player na barra de status, F6/F7/F8/F9. A saída usa um **driver Intel HD Audio próprio** (`hda.c`).
- **Teclado:** o firmware UEFI entrega sempre o layout US. O núcleo converte para **ABNT2** (ç, teclas mortas ´ ` ~ ^ ¨, AltGr+Q = `/`, AltGr+W = `?`), **US** ou **US-Internacional**.
- **Ponteiros:** mouse USB/PS2 (Simple Pointer, com aceleração) e touchpads/telas (Absolute Pointer: modo touchpad relativo com toque-para-clicar, ou modo absoluto detectado automaticamente).

## 3. Pen-drive = pen-drive comum

O pen-drive é **FAT32** puro, legível e gravável no Windows, Linux e macOS:

```
EFI\BOOT\BOOTX64.EFI, BOOTIA32.EFI   sistema
TEXTOS\                              textos e exportações (pasta padrão do gerenciador)
MUSICA\                              .mp3 (subpastas valem)
FOLIO\FONTES\                        fontes .ttf + licenças OFL
FOLIO\CONFIG.INI                     preferências (texto, editável)
FOLIO\RASCUNHO.MD                    recuperação automática
LEIA-ME.txt
```

## 4. Mapa do código

```
src/core/            núcleo, independente de plataforma (~7 mil linhas)
  folio.h            todas as declarações + a interface PAL
  base.c             libc mínima, heap (boundary tags), fmt, matemática, UTF-8, divisão 64-bit IA32
  gfx.c              primitivas AA, sombras, rasterizador de contornos
  font.c / embed.c   TrueType + kerning + cache + famílias / fonte embutida (.incbin)
  icons.c            ícones vetoriais e cursores
  theme.c            20 temas
  doc.c              documento (array de parágrafos), undo/redo, Buf
  md.c               Markdown (linha, inline, blocos)
  editor.c           layout, desenho, teclado, mouse, foco, busca
  preview.c          modo leitura
  export.c           exportadores + ZIP
  filedlg.c          gerenciador de arquivos + diálogos confirmar/perguntar
  music.c            player MP3
  settings.c         CONFIG.INI + tradução de layout de teclado
  app.c              barras, janelas, comandos, atalhos, laço principal (app_run)
src/platform/uefi/   efi.h (escrito à mão), uefi_main.c (GOP/teclado/ponteiros/FS/tempo), hda.c
src/platform/win32/  simulador + modo de roteiro headless que gera PNG
src/libc/            string.h / stdlib.h mínimos (para o minimp3)
third_party/         minimp3.h (CC0)
tools/efi_host.c     carregador PE32+ e firmware UEFI emulado para Windows
tests/*.txt          roteiros de teste
src/legacy, tools/legacy   FOLIO-86 v3 (Assembly) intacto
```

**Regra de ouro:** o núcleo só fala com o hardware pela PAL (`pal_*` em `folio.h`). Uma plataforma nova precisa apenas implementar a PAL.

## 5. Build e testes

```powershell
.\build.ps1            # compila tudo, monta build\usb_files e roda os testes
.\build.ps1 -NoTests
.\build.ps1 -Legacy    # também gera o folio86.img da v3 (precisa de nasm)
.\flash_usb.ps1 -DriveLetter E:     # instala/atualiza sem apagar arquivos do usuário
.\build.ps1 -Image -ImageMB 4096    # gera build\folio.img (MBR+FAT32) p/ Rufus/Etcher/dd; -Vhd gera .vhd p/ VirtualBox
.\build\folio_sim.exe               # experimentar no Windows (janela)
.\build\efi_host.exe build\efi\BOOTX64.EFI   # rodar o binário UEFI real numa janela
```

Os testes automáticos fazem o seguinte: (1) o simulador digita, testa a tipografia inteligente, as listas e o ABNT2 com teclas mortas, abre todas as janelas e gera capturas em `build\shots`; (2) exporta os 7 formatos, descompacta DOCX/ODT/EPUB e valida cada XML; (3) **carrega o `BOOTX64.EFI` real** no `efi_host` (parse do PE, relocações, System Table emulada), dá boot, digita pelo caminho de teclado do firmware e confere que o arquivo foi gravado no volume.

Flags de compilação UEFI importantes: `-ffreestanding -mno-red-zone -fshort-wchar -mno-stack-arg-probe -fno-tree-loop-distribute-patterns` (esta última evita que o GCC transforme o próprio `memset` em chamada a `memset`) e `-nostdlib -shared -Wl,--subsystem,10`.

## 6. O que NÃO foi validado e precisa de hardware real

Desde a v4.3 há um **laboratório QEMU** (`tools/qemu-lab.ps1`, 13 máquinas: UEFI x64/IA32/ARM64 e BIOS) que confere boot, teclado, clique, som, salvar e a integridade do volume em cada uma. Continua valendo testar em máquina física, porque firmwares reais variam:

1. **Boot em firmware real**: o laboratório usa OVMF/AAVMF/SeaBIOS; placas-mãe reais têm quirks (USB legacy, CSM, VBE com modos estranhos). O `DIAGNOSTICS.TXT` e o registro serial (`debug_serial=1`) são o caminho para depurar.
2. **Áudio (`hda.c`):** PCI e DMA pelo protocolo PCI I/O do firmware (funciona com IOMMU/proteção de DMA), snoop por fabricante, clflush, todas as controladoras e codecs, saídas alto-falante + fone + linha, EAPD e GPIO. **Cada boot grava `FOLIO\DIAGNOSTICO.TXT`** (ponteiros e o inventário completo de pinos do codec); Ajustes → Sistema tem "Testar som" e "Salvar diagnóstico". Riscos restantes: codecs com quirks de coeficientes (Realtek), controladoras SOF/DSP sem modo HDA legado.
3. **Ponteiros:** o FOLIO conecta todos os drivers do firmware (ConnectController), lê cada dispositivo uma vez só (ignora o agregador ConSplitter quando há dispositivos reais), redescobre mouses ligados depois do boot e tem modo de toque Automático/Touchpad/Tela de toque em Ajustes → Sistema. Touchpads I2C sem driver no firmware continuam sem funcionar; USB áudio, Bluetooth, SoundWire e amplificadores inteligentes por I2C (Cirrus CS35L41 de alguns HP/Lenovo) não são suportados.
4. **Ctrl/Shift/Alt em firmwares sem `SimpleTextInputEx`:** nesses, Shift+setas (seleção pelo teclado) não funciona, e os atalhos que dependem de Alt ou Ctrl com tecla de função também não (**Alt+M**, **Alt+F7/F8**, **Ctrl+F6**, Alt+1…9). A paleta (Ctrl+K, que chega como caractere de controle) alcança todos os comandos, e Esc/Tab/setas/Enter funcionam em qualquer firmware.
5. **Secure Boot:** os binários não são assinados. É preciso desativar o Secure Boot (ou assinar com shim/MOK — item do backlog).

## 7. Backlog sugerido

- Assinatura para Secure Boot (shim + MOK).
- ~~Port para BIOS legado~~: feito na v4.3 (ver abaixo). A imagem `.img` do `-Image` ainda é UEFI-only; rodar `build\biosboot.exe build\folio.img` a deixa híbrida.
- Feito na v4.0.1: só os trechos alterados vão para o vídeo (cópia-sombra), framebuffer em write-combining por MTRR (`video_rapido=0` no CONFIG.INI desliga), laço sem esperar o timer do firmware, memcpy/memset com `rep movsb/stosb`. Próximo passo, se preciso: redesenhar só retângulos sujos também na renderização (hoje ~5 ms por quadro a 1080p).
- Feito na v4.0.2: áudio — corrigido o teste de classe PCI (`cls >> 16`; antes nenhuma placa era reconhecida), backend AC'97, multi-link/GPROCEN em Intel SKL+, codecs não anunciados, buffer de posição (DPLBASE) quando o LPIB não anda, tentativa em todos os streams, toast com o motivo + DIAGNOSTICO.TXT. Vídeo — WC pela PAT (PA4=WC, páginas do framebuffer divididas; vence MTRR UC do firmware), cópia ao framebuffer com movntdq de 16 bytes, memcpy/memset em `rep movsq/stosq`, sombra com perfil por linha, folha de papel pré-desenhada no fundo (`app_paper_sheet`). Bench 1080p: rolagem 7,4→2,4 ms, janelas 15→5 ms. `bench N win` no simulador.
- Feito na v4.0.3 (vídeo universal): escolha da saída GOP por EDID ativo/framebuffer/console (2+ monitores = console espelhado), EDID ativo ou detectado (1º descritor de temporização), modo nativo ou maior com a mesma proporção, `resolucao=`/`rotacao=` no CONFIG.INI e em Ajustes > Sistema, formatos RGB/BGR/máscara 16-24-32 bits/só Blt, UGA Draw sem GOP, validação do framebuffer (IA32 >4 GB → Blt), rotação 0/90/180/270 (automática em painel retrato; toque convertido), Ctrl+Alt+setas/Home, escala pelo DPI do EDID, connect_all e nova tentativa se o fast boot não ligou a GPU. Testes: `efi_host --gop rgb|mask16|blt|pad|uga`, `--size 800x1280 --edid` (no build.ps1).
- Hifenização em PT-BR, verificação ortográfica offline, áudio por USB.

## v4.0.4 — qualidade de vida na leitura e na escrita
- Leitura ↔ escrita sincronizadas: `MdBlock.src` guarda o parágrafo de origem; `preview_open` abre no parágrafo do cursor, `preview_leave` (Esc/Ctrl+P) põe o cursor no parágrafo da "linha de leitura" (30% da altura). Mudar fonte/tamanho na leitura mantém o trecho.
- Leitura: ←/→ e clique nas margens viram página; Ctrl+PgUp/PgDn capítulos (também no editor); barra de status com % lido, capítulo e tempo restante (200 ppm); barra de rolagem; Sumário (F5) navega na leitura.
- Posição por documento: `prefs.recent_pos` (`posicaoN=p,o` no CONFIG.INI), `app_remember_pos()`; `lendo=1` reabre no modo leitura.
- Editor: Ctrl+J última alteração (ida e volta), Ctrl+D duplicar, Ctrl+Shift+K apagar parágrafo, Shift+F3 caixa, seleção + ( [ " * _ ` envolve; barra de rolagem fina com marcas de capítulo (H1/H2), arrastável. Paleta: "Inserir a data de hoje".
- Janelas: clicar fora fecha (modal do topo, ou todas as comuns; barras excluídas); o clique é consumido. Fechar o diálogo de confirmação sem responder = Cancelar (antes deixava `pend_action` armado).
- UEFI: com Ctrl conhecido, códigos 9/10 viram Ctrl+I/Ctrl+J (antes Ctrl+I chegava como Tab).
- Teste: `tests/leitura.txt` (comandos novos do simulador: `assert_para`, `assert_no_text`, `assert_nowin`).
- Barra superior: com faixa carregada, botões anterior · tocar/pausar · próxima à esquerda da clave.
- Página × tela cheia: `prefs.full_width` (`tela_cheia=` no CONFIG.INI); `app_sheet_on()` e `app_text_w()` centralizam folha e largura (editor, leitura, barras). Tamanhos prontos 560/700/860/1040 px em Ajustes > Aparência > PÁGINA e na paleta.
- Distribuição sem imagem: o build gera `build\FOLIO-4.zip` (~2,5 MB, só o sistema; entradas com "/" via ZipArchive, pois o CreateFromDirectory do PS 5.1 grava "\"). Basta descompactar na raiz de qualquer pen-drive FAT32; instruções em `docs\COMO-INSTALAR.txt` (também vai no ZIP e em usb_files). As imagens .img ficam só como alternativa.

## v4.0.5 — página rolando sozinha + rolagem automática na leitura (estudo em `docs\ESTUDO_ROLAGEM.md`)
- Bug: rolagem "fantasma" vinha da entrada. UEFI: filtro do eixo Z por dispositivo em `poll_simple` (valor absurdo, Z preso > 1,5 s, Z repetido junto com movimento = byte que não é roda); contadores no DIAGNOSTICO.TXT. Win32: `WM_MOUSEWHEEL` acumula deltas (inércia de touchpad contava como dente inteiro).
- Rolagem automática só por ação explícita: barra de leitura no centro da barra de status (página anterior · ▶/❚❚ · − ppm + · próxima página) ou clique do botão do meio; segurar e arrastar com o meio = rolagem estilo navegador. Velocidade em palavras/min (`leitura_ppm=`, 60–900), convertida pela densidade de palavras da janela de leitura; rampa de ~0,3 s; Esc para; para no fim do texto; pausa com janela aberta.
- Botão do meio no núcleo (`Input.mdown/mpressed/mreleased`, `button = 2`); no UEFI, esquerdo+direito juntos emulam o meio.
- Tempo de leitura restante usa `prefs.read_wpm`. Comandos novos do simulador: `mclick`, `mdown`, `mup`.
- Auditoria (mesma data) e correções: Esc durante arrasto do meio ligava a rolagem (agora `preview_stop_motion`, nunca liga); janela comum (música) pausava a rolagem (agora só modal/teclado/arrasto); linhas tremiam 1 px (deslocamento da página arredondado uma vez); filtro Z por densidade (a regra de 1,5 s bloqueava rolagem lenta legítima); quadros agendados por `ui.wake_ms` em vez de 140 fps; parada em ~0,1 s; respeita `ocultar_barras=0`; zen alcança a barra de baixo; tabelas contam palavras; "pal/min"; gravação da velocidade com atraso de 2 s; captura/foco do simulador; rede de segurança do arrasto (tecla/clique encerra). Novo teste `tests/efi_roda.txt` (comandos do efi_host: `zjunk`, `wheel`, `rclick`/`rrelease`, `assert_same`/`assert_diff`), validado com controles negativos. Simulador: `barclick`, `assert_still`, `assert_moving`, `assert_auto`.
- Paleta Ctrl+K: "Rolagem automática da leitura (ligar/desligar)" (`c_autoscroll`; fora da leitura, abre e liga) — caminho para PCs sem mouse no firmware. Testado em `tests/leitura.txt`.

## v4.0.6 — abas, temas do usuário, pacote de fontes e abertura FolioOS (16/11/2026)
- **Abas opcionais** (estilo Sublime): `prefs.tabs` (`abas=` no CONFIG.INI), ligadas pelo ícone `IC_TABS` na barra superior, pela paleta ou em Ajustes > Escrita. Desligadas = comportamento antigo. O documento ativo continua em `app`; os demais ficam em `tab_doc[]` (app.c, `stash_current`/`load_slot`). Sem nome → rascunho próprio `FOLIO\ABAS\ABA-N.MD`; com caminho → autosave no próprio arquivo. Lista em `abaN=vol|caminho` ou `abaN=*|N`, `abaposN=p,o`, `aba_ativa=`. Desligar grava tudo e guarda a lista; religar reabre (o documento atual entra na lista). Com abas, desligar/reiniciar não pergunta (tudo vai para o disco). Atalhos: Ctrl+N/W, Ctrl+Tab (no UEFI pode virar Ctrl+I), Alt+1…9, botão do meio fecha. As abas ocupam o lugar do título na própria barra superior (entre o menu e os ícones da direita, `draw_tabbar` chamado por `draw_topbar`); não há faixa extra, então o editor não muda de altura. A aba atual nunca some quando não cabem todas.
- **Temas do usuário**: `FOLIO\TEMAS\*.INI` (theme.c: `themes_scan`, `theme_save_custom`, `theme_delete_custom`); cores `#RRGGBB` com as chaves de `ckeys[]`, `fonte=`, `fonte_interface=`, `folha=`, `vinheta=` (%). Tema com fonte de texto troca `prefs.font` ao ser escolhido; a da interface (`font_set_ui`) vale enquanto o tema está ativo. Editor de temas (`win_theme_ed`, `ui.sticky` = não fecha ao clicar fora) edita uma cópia que vira `T` ao vivo; Esc/Cancelar restaura. Exemplo em `assets\temas\Tinta azul.INI` (copiado só se não existir).
- **Fontes**: `fonts_scan` entra em subpastas (2 níveis), até 64 famílias. `assets\fonts-extras` (11 famílias OFL/Apache, ~11 MB) vira `build\FOLIO-FONTES-EXTRAS.zip` → `FOLIO/FONTES/EXTRAS/<família>`. Literata, Lora e EB Garamond são fontes variáveis (o FOLIO usa a instância padrão e sintetiza o negrito).
- **Abertura FolioOS** (`splash.c`): desenhada logo após `fonts_init` (fonte embutida), antes da leitura de FOLIO\FONTES; dura `abertura_segundos=` (padrão 10, 1–60; lido direto do CONFIG.INI em `wanted()`, pois as prefs ainda não carregaram; slider em Ajustes > Sistema) e some com fade; tecla pula e segue para o editor, clique pula e é consumido. `abertura=0` desliga.
- Testes novos: `tests\abas.txt`, `tests\temas.txt` (simulador: `restart`, `assert_tabs`, `assert_active`, `themename`, `assert_theme`, `assert_font`, `assert_uifont`, `theme_save_copy`, `assert_splash`). Controle negativo: com `flush_doc` desativado, abas.txt falha ("1 abas, esperado 3").
- NÃO verificado em hardware: abertura e abas no firmware real (só efi_host), Alt+número/Ctrl+W pelo teclado UEFI, tempo de boot com o pacote de fontes inteiro.

## v4.1 — FolioOS universal: i18n sem nada fixo no código, código aberto (16/11/2026)
- **Texto-fonte em inglês**, marcado com `L()`, `N_()` (tabelas estáticas; `L()` no desenho) e `LN(um, vários, n)`. `i18n.c` descobre `FOLIO/LANGUAGES/*.po` no boot (só o cabeçalho, 4 KB), lê `Plural-Forms` (avaliador de expressão C), `X-Language-Name`, `X-Keyboard`, `X-Quotes`, `X-Fonts`, `X-Date-Format`, `X-Decimal`. Tradução com especificadores diferentes do original é descartada ao carregar. `fmt` aceita posicionais (`%2$s`).
- **Teclados como dados** (`keyboard.c`, `FOLIO/KEYBOARDS/*.txt`: `name/map/altgr/dead/compose/include`); 14 layouts gerados em `assets/keyboards`.
- **Caminhos** em `paths.c` (nomes em inglês; pasta antiga em português tem prioridade se existir — nada é renomeado). Chaves do CONFIG.INI e dos temas em inglês, antigas aceitas (`legacy_key`, `ckeys_old`, nomes antigos de temas em `theme_find`).
- **Idioma no boot** (`setup_language`): `language=` > CONFIG antigo sem `language` = pt-BR + abnt2 > `PlatformLang` da UEFI (`pal_lang`) > inglês. Idioma com fontes ausentes (CJK sem pacote) é recusado.
- **Fontes de idioma** sob demanda (`fonts_set_lang`), em `FOLIO/FONTS/_LANGUAGE` (pastas com `_` não entram na varredura do boot). Pacote `FOLIO-FONTS-CJK.zip` via `tools/fetch-cjk-fonts.ps1`.
- **Ferramenta** `tools/i18n.c` (`extract/update/check`); `build.ps1` regenera `folio.pot`, valida todos os `.po` e roda controle negativo. Listas de tradução em lote: `tools/po_from_list.pl` + `tools/build_po.sh`.
- Testes: os antigos rodam com `FOLIO_PLATFORM_LANG=pt-BR` (o simulador e o efi_host leem essa variável); novos `tests/i18n*.txt`.
- Não verificado em hardware: `PlatformLang` em firmwares reais, CJK com fontes de 10–17 MB na RAM da UEFI, layouts novos no teclado físico.

## v4.2 — uso 100% sem mouse (17/11/2026)
- **Navegação por teclado genérica** em `ui.c`: cada widget de janela se registra com `ui_nav(r, tipo)` (`NAV_ITEM`, `NAV_ADJ`, `NAV_TEXT`, `NAV_LIST`), na ordem do desenho; `ui_item(r)` é o item clicável genérico (clique **ou** Enter/Espaço). `nav_pre` (antes do corpo) trata Tab e Enter/Espaço/←→ do item focado; `nav_post` (depois) usa as setas que a janela não consumiu para andar entre os itens. `ui_reveal(&scroll, area)` rola até o item focado. O contorno só aparece com `ui.nav_show` (some ao clicar). **Todo elemento clicável novo deve usar `ui_item`/`ui_nav`**, senão quebra o uso sem mouse — `tests/teclado.txt` cobre.
- **Foco de teclado em painéis**: janela aberta por atalho (ou pela paleta/menu com Enter) recebe o teclado (`focus_new_top`); F5/F6/F10/F12 com o painel aberto sem o teclado devolvem o teclado a ele (`panel_key`); letras digitadas num painel voltam ao texto. Ctrl+F6 = `wm_cycle_focus`; Alt+M = menu; Alt+F7/F8 = `wm_kb_key` (mover/redimensionar).
- **Mouse desligado**: `prefs.no_mouse` (`use_mouse=0`; a chave antiga `mouse=` é a velocidade do ponteiro!). `app_event` ignora movimento/clique/roda.
- Alt+letra não passa pelo layout (atalhos iguais em AZERTY etc.) e não digita no editor; no simulador Win32, Alt+letra/número agora chega ao app.
- `tools/i18n.c update` preserva as referências `#:` e os comentários de tradutor (antes apagava). `tools/po_fill.pl` preenche traduções em lote a partir de uma lista simples.
- **Métodos de entrada** (`ime.c`): coreano 2 conjuntos (sílaba montada a cada jamo, Backspace desfaz por jamo) e japonês romaji → hiragana/katakana. O IME escreve por um `ImeTarget` (apagar N caracteres antes do cursor + inserir), usado pelo editor e pelos campos. Layouts ganham `ime hangul|kana` e `fonts …`. Ctrl+Espaço alterna. `font.c` carrega sob demanda a fonte CJK certa quando o texto tem hangul/kana/han (`lazy_cjk`). Teste: `tests/ime.txt`. Falta: kanji e pinyin (dicionários).
- **Fontes embutidas** (pedido do usuário: idiomas não são opcionais): `build.ps1` copia `assets/fonts-extras` → `FOLIO/FONTS/EXTRA` e `assets/fonts-cjk/_LANGUAGE` → `FOLIO/FONTS/_LANGUAGE` (baixa as CJK com `tools/fetch-cjk-fonts.ps1` se faltarem; sem elas o build falha). Não existem mais os pacotes FOLIO-FONTS-*.zip.
- READMEs em 15 idiomas com o **mesmo conteúdo** do README.md: `tools/readmes.pl` divide o inglês em blocos (o de código também) e lê a tradução de cada bloco em `docs/i18n/README.<código>.po`; títulos traduzidos ganham `<a id>` com a âncora do inglês. Mudou o README.md → rode `perl tools/readmes.pl` (blocos novos/mudados saem em inglês até alguém traduzir; `--check` lista). `import <código> <arquivo>` traz traduções em lote (blocos separados por `@@@`, todos ou só os que faltam). Nunca editar `README.<código>.md` à mão.

## v4.3 — FolioOS universal: BIOS, ARM64 e drivers próprios (17/11/2026)
- **Laboratório QEMU** (`tools/qemu-lab.ps1`, QEMU em `C:\Program Files\qemu`): 13 máquinas (`-List`), controle por QMP (screendump, send-key, input-send-event), som capturado em WAV, disco lido depois com `build\fatcat.exe` e conferido com `build\fat32_test.exe <img> --fsck`. Colunas: boot, teclado, mouse (clique no botão do menu), som, salvar (digita "folio lab saved", Ctrl+S, Enter, confere `DOCUMENTS/Welcome to FolioOS.md`), volume, diagnóstico. `-Only <regex>`, `-BootWait <s>`. Relatório em `build\lab\report.md`; por máquina: PNGs, `serial.txt`, `DIAGNOSTICS.TXT`, `fsck.txt`.
- **Registro serial**: `debug_serial=1` no CONFIG.INI → `dbg()` (folio.h) escreve pelo `EFI_SERIAL_IO_PROTOCOL` (e pela COM1 na ponte BIOS).
- **Correções achadas no laboratório**: heap IA32 (cabeçalho `HBlk` de 8 bytes quebrava o alinhamento de 16 — `_Static_assert` agora); HDA parava depois da 1ª resposta do codec (RIRBCTL precisa do bit RINTCTL e o RIRBSTS é limpo por escrita de 1); clique rápido de mouse se perdia (trava de botões `latch` nos drivers e na ponte); `flash_usb.ps1` não copiava `FONTS\EXTRA` nem `FONTS\_LANGUAGE`.
- **Drivers próprios de ponteiro** (só entram quando o firmware não oferece ponteiro de verdade): `usbhid.c` (EFI_USB_IO: descritor de relatório HID com IDs, botões/X/Y/roda, absoluto ou relativo; protocolo "boot" quando o descritor falha; publica SIMPLE/ABSOLUTE_POINTER virtuais) e `ps2mouse.c` (i8042, IntelliMouse com roda; só em ring 0 — no `efi_host` daria instrução privilegiada).
- **ARM64**: `BOOTAA64.EFI` com clang/lld (`--target=aarch64-unknown-windows`, `-fno-math-errno`, `-mno-stack-arg-probe`). Código x86 isolado por `FOLIO_X86` (efi.h): contador `cntvct_el0`, `dc civac` no lugar de clflush, sem portas (PCI só pelo PciIo), sem MTRR/PAT. `memcpy/memset` em C com barreira (o clang trocaria o laço por chamada recursiva). Testado em QEMU `virt` com Cortex-A72 (o do Raspberry Pi 4), virtio-gpu (só Blt) e ramfb, USB e HDA.
- **BIOS** (`src/platform/bios`): `boot.asm` gera o MBR (440 bytes; acha a partição ativa ou a 1ª FAT32 — pendrives do Windows costumam vir sem partição ativa) e o código do VBR (420 bytes; segue a cadeia de `FOLIO\BIOS.SYS`; **pula o rótulo do volume**, que também se chama FOLIO). `stub.asm` (início do BIOS.SYS em 0x8000): A20, E820, modo protegido 32 bits sem paginação, `rm_int` (volta ao modo real para INT 13h/10h; mascara IRQ1/IRQ12 para o BIOS não roubar bytes do i8042; CF salvo logo depois do `int`). `bridge.c` (ligado em 0x10000 por `bridge.ld`, clang ELF i686 com `-malign-double` para o layout de UINT64 bater com a UEFI): páginas e pool sobre o E820, eventos e Stall por TSC calibrado no PIT, teclado+mouse PS/2 num só lugar (o i8042 tem um buffer só: um byte de mouse não lido trava o teclado; por isso o mouse é da ponte, publicado com caminho de dispositivo para o FolioOS não ligar o `ps2mouse.c` por cima), GOP sobre VBE (modos lineares 24/32 bits; modo inicial pelo EDID), FAT32 sobre INT 13h (LBA ou CHS, buffer em 0x70000), CMOS, ACPI S5 para desligar, COM1. Marca o protocolo RootBridgeIo para o `hda.c` usar as portas 0xCF8. Carrega `BOOTIA32.EFI` (PE32 + relocações) numa pilha de 1 MB.
- **`fat32.c`** (independente de plataforma, escrita direta): nomes longos (leitura e criação, alias ~N), criar/apagar/renomear, crescer/truncar, duas FATs, FSInfo mantido em memória. `tools/fat32_test.c` faz milhares de operações aleatórias contra um modelo, remonta e confere o volume inteiro (o build roda).
- **Instalação do boot BIOS**: `tools/biosboot.c` → `FOLIO\TOOLS\biosboot.exe E:` (ou numa imagem). Só grava bytes 0..2 e 90..509 do setor de boot (e da cópia no setor 6) e os 440 bytes de código do MBR; confere antes que é FAT32 com `FOLIO\BIOS.SYS`. `flash_usb.ps1 -Bios` chama o mesmo. Falta testar `biosboot.exe` num pendrive de verdade (precisa de administrador).
- Ainda não validado em hardware físico: Raspberry Pi com firmware UEFI da comunidade; PCs só-BIOS reais (emulação USB legacy de teclado/mouse, VBE de placas antigas).
- **Contribuições (PRs #4 e #5, de @jagalindo)**: simulador nativo para macOS (`src/platform/macos/macos_main.m`, `build_macos.sh --test` roda os mesmos roteiros) e edição de fontes LaTeX (`src/core/tex.c`: sintaxe por documento em `AppDoc`, `md_on()` desliga tudo que é Markdown, realce por linha, CRLF preservado, autosave só para `.md/.txt/.tex/.bib/.sty/.cls` via `path_editable`). Teste `tests/latex.txt` no `build.ps1`.
- Atenção ao testar fora da pasta do projeto: o "Acesso controlado a pastas" do Windows bloqueia programas novos de criar .docx/.odt/.rtf/.txt dentro de Documentos (as exportações ficam como `.tmp`). Pastas de teste em `%LOCALAPPDATA%` não têm o problema.
