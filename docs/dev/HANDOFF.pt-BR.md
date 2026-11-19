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

## Instalador e dual boot (2026-11-18) — estado para retomar

Fases 1, 2 e 3 completas (ver o fim desta seção). `.\build.ps1` passa inteiro, zero warnings; o laboratório QEMU passou nas 15 configurações + a seção dual (rodar em partes: `-Only "^(q35|pc-)"`, `-Only "^(ia32|aa64|bios)"`, `-Only "^(inst|dual)"`, cada parte cabe em 10 min).

**Feito — fase 1 (instalar como único sistema):** `src/core/part.c` (GPT/MBR; a GPT regravada mantém cada entrada no seu slot, `Part.slot`), `src/core/install.c` (`install_whole_disk`), `src/core/disks.c` (discos no DIAGNOSTICS.TXT), `src/core/installer.c` (tela, só teclado, palavra de confirmação traduzível `L("ERASE")`), PAL `pal_disk_*` (UEFI BlockIo/DiskInfo; ponte BIOS INT 13h por drive; simulador `FOLIO_DISKS=a.img;b.img`; `efi_host --disk img`). `fat32.c` foi para `src/core`. Bug corrigido: rótulo "FOLIO" virava colisão de nome curto (pasta FOLIO~1 quebrava o boot BIOS).

**Feito — fase 2 (ao lado de outro sistema, UEFI):** `install_alongside` (partição FAT32 "dados básicos" no espaço livre + `\EFI\FolioOS\{BOOTX64.EFI,FOLIO.ID}` na partição EFI existente), `pal_boot_entry_add` (Boot####/BootOrder; também no disco inteiro), `app_init` troca para a partição de dados pelo `FOLIO.ID` + `VolInfo.part_guid`, `src/core/chooser.c` (seletor: `boot_wait=` no CONFIG.INI; sem tecla usa BootNext + reinício, nunca LoadImage — BitLocker/TPM não vê o FolioOS). Bug corrigido: `prefs_save` perdia `debug_serial`. Testes: `install_test --windows/--alongside/--compare/--winefi`, `tests/instalar*.txt`, `tests/seletor.txt` (simulador: `FOLIO_VOL_GUID`, `FOLIO_OTHER_OS`), `tests/efi_instalar.txt` (efi_host ganhou `key tab|shift+tab|enter|esc`), lab `dual-uefi-sata` com `tools/fakewin.c` (Windows de mentira: tela azul + "FAKE WINDOWS STARTED"). `fatcat` aceita `FATCAT_VOL=N`.

**Feito — recusas da fase 2 (18/11, sessão como administrador):** `src/core/fscheck.c/.h`: leitor NTFS só leitura (MFT com fixups, runlists, `$ATTRIBUTE_LIST`, `$Volume` sujo, `hiberfil.sys` começando por "hibr" = hibernado/Inicialização Rápida, `$Bitmap` → último cluster usado) e `fs_check()` com vereditos SAFE_*. Na dúvida o leitor responde "ilegível", nunca "pode mexer": a busca do `hiberfil.sys` só diz "não existe" depois de ler o índice inteiro (blocos INDX em uso, pelo `$BITMAP` do índice).
- `tools/ntfs-fixtures.ps1` (precisa de administrador; rodar só para refazer): VHDs de 48 MB formatados pelo próprio Windows → `tests/fixtures/ntfs-{clean,dirty,hiber}.img.gz` (~260 KB cada, commitáveis) + `.txt` com o que o Windows informa (FSCTL: cluster, total, início da MFT, último cluster usado). O build descompacta e não precisa de administrador.
- `tools/fscheck_test.c` no `build.ps1`: o leitor tem de concordar com o Windows; controles negativos (setor da `$Volume` estragado, `$Bitmap` estragado, tamanho de setor trocado → "ilegível"); sintéticas BitLocker/LUKS/LVM/APFS/HFS+/ext4 (limpo, diário, erros)/FAT32/XFS/NTFS vazio, em setor de 512 e de 4096.
- Bugs que as fixtures pegaram: (1) a raiz de um volume real tem `$ATTRIBUTE_LIST`, o índice fica num registro de extensão — o leitor não achava o `hiberfil.sys` e dizia "não hibernado"; (2) o último byte do `$Bitmap` tem os bits de sobra ligados (último cluster = nclus); (3) ext com setor de 4096 estourava um buffer de 512.
- Instalador: cada partição NTFS/ext mostra o estado quando pede ação ("Windows hibernado ou Inicialização Rápida", "precisa de verificação (chkdsk)"…); ao lado de um disco com BitLocker, aviso em vermelho + interruptor "Tenho a chave de recuperação do BitLocker", sem ele o botão não instala. Testes: `install_test --bitlocker`, `tests/instalar_bitlocker_nao.txt` (disco byte a byte igual) e `tests/instalar_bitlocker.txt` (instala). 21 textos novos traduzidos nos 14 idiomas.

**Feito — encolher NTFS sem mover dados (18/11):** `ntfs_shrink()` no `fscheck.c`. Segue as convenções do Windows: partição = total do BPB + 1, cópia do setor de boot no último setor, `$Bitmap` com tamanho múltiplo de 8 bytes e os bits de sobra ligados, `$BadClus:$Bad` como uma run esparsa do tamanho do volume. Só aceita volume em ordem (nem sujo, nem hibernado, `$LogFile` limpo, sem clusters defeituosos). Ordem das gravações, para sobreviver a queda de energia: cópia do boot no novo fim → `$Volume` sujo (+ `$MFTMirr`) → `$BadClus` → `$Bitmap` → setor de boot. O volume fica marcado para o chkdsk. `install_shrink_alongside()` faz a cópia de segurança, encolhe, ajusta a entrada da tabela e instala no espaço liberado.
- Tela: sem espaço livre, aparece a opção "ao lado, abrindo espaço na partição NTFS". O outro sistema fica com o que usa + 10%; o tamanho do FolioOS vai no deslizante (←/→), padrão 4 GB, com prévia "NTFS: X → Y".
- Testes no build: `fscheck_test` (encolhe a fixture limpa, recusas sem mudar um byte, `$MFTMirr` igual à MFT, bytes depois do novo fim intactos), `install_test --shrink-ntfs` na fixture `big` (disco de 1100 MB feito pelo Windows, EFI + NTFS sem espaço livre), `tests/instalar_encolher.txt` + `fscheck_test --shrunk`.
- Conferido pelo próprio Windows: `tools/ntfs-shrink-check.ps1` (administrador) monta como VHD o volume encolhido e o disco instalado: `chkdsk` só leitura e `/f` sem problemas, todos os arquivos com o mesmo hash, arquivo novo gravado e relido, FAT32 do FolioOS legível e com `chkdsk` limpo. Passou em 30 MiB e no limite (26 MiB).
- Imagens grandes: `FOLIO_WORK=E:\FolioOS-work` faz o build, o laboratório e os scripts das fixtures usarem o disco E: (o usuário autorizou; o C: tem pouco espaço).

**Feito (18/11, depois):** Ajustes → Sistema → INICIALIZAÇÃO mostra a espera do seletor (0–30 s) só quando o FolioOS iniciou ao lado de outro sistema (`alongside` em `app.c`); teste `tests/seletor_ajuste.txt` (o build confere `boot_wait` no CONFIG.INI). `fs_check` lê a marca de "sujo" do FAT16/FAT32 (entrada 1 da FAT + byte de estado do BPB) e do exFAT (`VolumeFlags`); o instalador mostra o estado de todas as partições e não grava numa partição EFI suja (`esp_clean` no motor, antes de encolher qualquer coisa). Laboratório: `dual-ntfs-shrink` (fixture `big` + `install_test --put-winefi build\fakewin.efi`), instala pela tela encolhendo, confere com `fscheck_test --shrunk`, inicia o "Windows" sem tecla e o FolioOS com tecla — tudo OK, assim como `dual-uefi-sata` e `inst-*`.

Observação: num build, um teste acusou 1 bit diferente (0x97 ↔ 0x9F) numa imagem de 1,4 GB, numa região que nada grava; refeito, o disco ficou idêntico. Não é o FolioOS — suspeita de RAM/disco sob pouca memória livre. Se voltar a acontecer, vale rodar o Diagnóstico de Memória do Windows.

**Linux como juiz (18/11):** `tools/linux-run.ps1` inicia o Alpine 3.24.4 "virt" no QEMU (ISO baixado para `$FOLIO_WORK\alpine`, conferido por SHA-256; nada é instalado no Windows), conversa pela serial (TCP), instala o e2fsprogs do próprio ISO e roda um script contra a imagem de disco (`/dev/vda`). ~40 s por execução. `tools/ext-fixtures.ps1` + `tools/ext-fixture.sh` geram `tests/fixtures/ext-{clean,small,errors,recover}` (GPT via `fscheck_test --mkgpt`, `mkfs.ext4` do Linux, fatos do `dumpe2fs`); o `fscheck_test` confere os vereditos (os 4 batem, inclusive blocos de 1 KiB).

**Descoberta sobre ext4:** o `mkfs.ext4` põe o diário no meio do volume e o Linux espalha as pastas pelos grupos de blocos (no volume de 256 MB, `/home` foi para o 2º grupo e há dados no bloco 64952 de 65024). Encolher ext4 sem mover dados quase nunca libera espaço; encolher de verdade exige mover blocos (como o `resize2fs`). **Decidido pelo usuário: o FolioOS não redimensiona ext4.** Sem espaço livre num disco com Linux, a tela oferece só o disco inteiro e mostra a dica de encolher pelo próprio Linux (GParted) e abrir o instalador de novo; o mesmo para NTFS sem folga (Gerenciamento de Disco). Fixture `ext-linux` (EFI + ext4 de 600 MB feito pelo Linux, `fscheck_test --mkgpt ... --esp`) + `tests/instalar_linux.txt` (o build confere que o disco não muda).

**Feito — desinstalar (18/11):** `install_find` (acha a instalação ao lado pelo `\EFI\FolioOS\FOLIO.ID`) e `install_remove`: cópia de segurança → textos (`DOCUMENTS`) para o pendrive em `DOCUMENTS/computer-<data>/`, conferidos relendo → apaga `\EFI\FolioOS` (só se a EFI estiver limpa) → tira a entrada da tabela e aumenta a do NTFS → `ntfs_grow` (o inverso do encolher; o `$Bitmap` não ganha clusters, `ntfs_grow_max`). O `FOLIO.ID` agora guarda `ntfs=<início>,<tamanho>` do NTFS antes de encolher, e a desinstalação volta exatamente a ele. `part_write` preserva o MBR de proteção existente (CHS do Windows). `pal_boot_entry_remove` (UEFI; simulador imprime "boot entry removed"). Tela: opção "Remover o FolioOS" (1ª da lista quando há instalação; também na paleta), prévia, interruptor "copiar os textos" (desligado pede a palavra de confirmação). Testes: `fscheck_test` (encolher + crescer = volume byte a byte igual, fora a marca do chkdsk), `install_test --remove` e `--same-table`, `tests/remover.txt`; `tools/ntfs-shrink-check.ps1` etapa 3: o Windows vê só o NTFS, mesmos arquivos, chkdsk limpo, tabela/MBR/boot NTFS idênticos aos que o Windows fez.

**Pendente:** laboratório `dual-ntfs-shrink` passos D/E (remover pela tela num OVMF de verdade, depois o firmware tem de iniciar o "Windows" direto). Na 1ª tentativa o E caiu no UEFI Shell: o log mostrou que o OVMF já tinha tirado da BootOrder as entradas do disco (ele reconstrói a ordem pelo `bootindex` do QEMU, e só o pendrive tinha). Corrigido no laboratório (o disco ganha `bootindex=1` quando o pendrive está ligado) e no FolioOS (`pal_boot_entry_remove` também apaga `Boot####` do FolioOS fora da BootOrder). Rodado de novo (18/11): `dual-ntfs-shrink` tudo OK — o log mostra `boot entry remove: Boot0004 "FolioOS"`, as outras mantidas, e no passo E o OVMF inicia o disco e o "Windows" direto, sem seletor. `dual-uefi-sata` também OK.

Atenção — integridade de dados nesta máquina: duas falhas passageiras hoje, ambas em arquivos grandes no disco E: com pouca memória livre (1 bit trocado numa imagem de 1,4 GB; o `mkimg` releu um arquivo de fonte diferente do gravado). Repetidas, deram certo. Sugerido ao usuário rodar o Diagnóstico de Memória do Windows e `chkdsk E:`.

**Feito — o resto da fase 1 (18/11):**
- `FOLIO/INSTALLED.ID` (tipo `whole`/`alongside` e `version=`) gravado pelas duas instalações; `install_find` reconhece as duas (o disco inteiro também sem o arquivo, pelo rótulo FOLIO + pastas, em instalações antigas).
- Remover a instalação de disco inteiro: textos para o pendrive, tabela e começo do volume zerados (o disco fica vazio); a palavra de confirmação é sempre pedida. `pal_boot_entry_remove` reconhece também a entrada pela descrição "FolioOS" (a do disco inteiro aponta para `\EFI\BOOT`).
- Atualizar por um pendrive mais novo (`install_update`): só programa, fontes, idiomas, temas e teclados (o `skip()` com `updating` deixa DOCUMENTS, MUSIC, VERSIONS, TABS, DRAFT.MD, CONFIG.INI e INSTALLED.ID); cada arquivo via `copy_safe` ("<nome>.~new" → confere por CRC relendo → apaga o antigo → renomeia); ao lado, `\EFI\FolioOS\<programa>` também. Bug achado no caminho: `fat_lookup(f, &n, w, &n)` (a pasta como saída) quebrava — o driver agora copia a pasta antes.
- Onde ficam os textos: `docs_place=computer|usb|both` (CONFIG.INI; o instalador grava pela escolha "Seus textos", `write_docs_place` no motor, e acerta o CRC da lista conferida). No app: `app_docs_vol/app_docs_dir` (Documentos da janela de arquivos no pendrive com `usb`), `app_exchange_texts` (`both`: o mais novo vale, o substituído vai para FOLIO/VERSIONS pelo `snapshot`, nada é apagado, texto aberto em outra aba fica de fora, o ativo é recarregado), seção TEXTOS em Ajustes → Sistema (só numa instalação), comando na paleta, aviso na inicialização sem o pendrive.
- Tela: paleta "Atualizar…/Remover…" abre com a opção já marcada; janela do instalador 680 de altura. Ordem do teclado: no disco inteiro a escolha dos textos é chamada depois dos botões (o roteiro do laboratório não mudou); ao lado, antes (roteiros do BitLocker e de encolher ganharam um Tab).
- Testes: `install_test --update`, `--remove` (também disco inteiro), `--docs-stick/--docs-both`; `tests/atualizar.txt`, `remover_disco.txt`, `textos_pendrive.txt`, `trocar.txt` (simulador com `--vol2`). Laboratório inteiro verde (17 configurações).

Até aqui está no commit 1e28c6b (branch `feature/install-dual-boot`, não enviado). O que vem abaixo foi feito depois.

**Feito — FAT32 e exFAT encolher/crescer (fase 2):** `fat32_resize` e `exfat_resize` no `fscheck.c` (só o fim muda; FAT32: total do BPB, cópia do boot no setor 6, FSInfo, ≥ 65525 clusters, a FAT tem de caber; exFAT: VolumeLength, ClusterCount, checksum da região de boot principal e de reserva, tamanho do bitmap). `fs_resizable/fs_resize/fs_grow_max` unificam NTFS/FAT32/exFAT para o instalador, a desinstalação (`shrunk=<início>,<tamanho>` no `FOLIO.ID`/`INSTALLED.ID`; o antigo `ntfs=` continua aceito) e o gerenciador. `fat32_format2` dimensiona a FAT para crescer até 4× (ou até o fim do disco). Fixtures feitas pelo Windows `tests/fixtures/{fat32,exfat}-{data,pc}`; juízes: `chkdsk` (`tools/ntfs-shrink-check.ps1` etapa FAT32/exFAT, `tools/vhd-chkdsk.ps1`) e `fsck.vfat`/`fsck.exfat` do Alpine (`tools/linux-run.ps1`; repositórios por http + `apk update`).

**Feito — gerenciador de partições (fase 1):** `src/core/partitions.c`, janela "Discos e partições" (Ajustes → Sistema e paleta): criar FAT32 no espaço livre, formatar, redimensionar NTFS/FAT32/exFAT (o volume antes da tabela ao encolher; a tabela antes do volume ao crescer), apagar com palavra de confirmação. Motor: `part_op_create/delete/format/resize` no `install.c`; testes `install_test --op-*`, `tests/particoes.txt`, `tests/particoes_apagar.txt`. A regra do "diário" do ROADMAP foi trocada pela ordem segura das gravações (anotado lá).

**Feito — fase 3, dual boot em PCs só BIOS:**
- `part.c`: no MBR cada primária mantém a sua posição (`Part.slot`), as novas vão para a primeira livre, e uma entrada que não mudou é regravada com os 16 bytes originais (CHS incluído).
- `install.c`: `alongside_mbr` (primária FAT32 no espaço livre, ativa continua a do outro sistema; `FOLIO/OTHERMBR.BIN` = 440 bytes do código original + nome "Windows"/"Linux"; VBR do FolioOS na partição; por último o MBR do FolioOS com o número da entrada no byte 437). `install_shrink_alongside` aceita MBR. `find_alongside_mbr` acha a instalação pelo `OTHERMBR.BIN`; `find_neighbor` (comum a UEFI e BIOS) acha a partição que cresce de volta. `install_remove` em BIOS: textos → código original de volta ao MBR → tira a entrada → cresce a vizinha. `install_update` não mexe em partição EFI quando não há.
- `boot.asm`: o MBR inicia a entrada do byte 437 (0xFF = a ativa, como antes). `stub.asm`: `rm_chain` (modo real, DL = disco, salta para 0:7C00). `bridge.c`: protocolo `FOLIO_BIOS_CHAIN` (põe o código original + a tabela atual em 0x7C00 e chama `rm_chain`). `chooser.c`: sem `BootNext`, usa `OTHERMBR.BIN` e `pal_chain_mbr`. `app.c`: "ao lado" também pelo `OTHERMBR.BIN`.
- Testes: `tools/fakeother.asm` (MBR de mentira que escreve FAKE OTHER STARTED na serial), `install_test --windows-bios <mbr>` e `--mbr-fat32 <mbr>` (instalar ao lado / encolhendo, atualizar, remover, `--same-table`; no build). Laboratório `dual-bios-ide` (SeaBIOS): instala pela tela, sem tecla inicia o "outro sistema", com tecla o FolioOS, remove pela tela, e depois o MBR original roda direto — tudo OK.

**Validado e commitado (18/11):** depois de reiniciar, `.\build.ps1` passou inteiro (zero warnings) e `.\tools\qemu-lab.ps1 -Only "^dual"` passou nas 3 configurações (`dual-uefi-sata`, `dual-ntfs-shrink`, `dual-bios-ide`). Tudo acima está no commit seguinte ao 1e28c6b, na branch `feature/install-dual-boot`, sem push.

**Feito — opção "manter proporção" (prometida a um usuário do Reddit, 18/11):** quando o modo de vídeo tem outra proporção que o painel (ex.: 1024×768 num 16:9) e o monitor estica a imagem. A ideia inicial de barras pretas foi descartada: o monitor estica o sinal inteiro, barras incluídas, então os pixels continuariam deformados. Em vez disso, o app desenha numa tela lógica com a proporção do painel (1024×576), que o `uefi_main.c` amplia para o modo com interpolação linear (`emit_scaled`; numa direção a escala é 1). O monitor estica e tudo volta ao formato certo, a imagem ocupa a tela inteira e o ponteiro absoluto (coordenadas 0..1) não muda. Serve à UEFI e à ponte BIOS (o mesmo `uefi_main.c`), com rotação. Ajustes → Sistema → Tela: segmentado "Proporção: desligada / Monitor (EDID; sem EDID, 16:9) / 16:9 / 16:10 / 4:3", `keep_aspect=off|monitor|16:9|16:10|4:3` no CONFIG.INI, 3 frases nos 14 idiomas; Ctrl+Alt+Home desliga. `pal_set_aspect` + `pal_video_apply` (o `app_init` aplica os ajustes de vídeo antes de gravar o DIAGNOSTICS.TXT; antes, `rotation=` do CONFIG.INI só aparecia lá depois do boot). DIAGNOSTICS: linha "keep proportions: drawn at …, stretched to …". Testes: `build.ps1` (efi_host 1024×768 com framebuffer/Blt/máscara 16 bits, 0° e 90°; "monitor" com EDID igual ao modo não muda nada) e laboratório `aspect-uefi-q35-std` e `aspect-bios-pc-std` (`ini=` por configuração), ambos OK. Falta só o retorno do usuário em hardware real (pedir o `DIAGNOSTICS.TXT`).

**Não depende de nós:** fase 7 (Secure Boot: shim assinado pela Microsoft precisa de certificado EV e conta de desenvolvedor de hardware do mantenedor) e o FolioOS Sync (fases 4–6, outra edição).

Cuidados: disco C: chegou a encher com imagens de teste — usar `FOLIO_WORK=E:\FolioOS-work` (o lab sem `-Keep` apaga as imagens). Edições com barra invertida pelo Bash se perdem: usar Edit/Write ou scripts perl gravados com Write.
