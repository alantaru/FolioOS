# FOLIO 4 — estudo da rolagem (página que rolava sozinha e rolagem automática)

*15/11/2026 · revisado após auditoria · código em `src/core/preview.c`, `src/platform/uefi/uefi_main.c` (poll_simple / poll_pointers), `src/platform/win32/win32_main.c` e `tools/efi_host.c`.*

## 1. A página que rolava sozinha

O FOLIO **não tinha** rolagem automática, e o núcleo não move o texto sem entrada: no simulador, a leitura parada fica idêntica por segundos (agora verificado por `assert_still`, que mede a posição de rolagem). A rolagem fantasma, portanto, vem da **entrada interpretada como roda do mouse**. Há duas fontes, uma confirmada e uma provável:

| Plataforma | Causa | Situação | Correção |
|---|---|---|---|
| Windows (simulador) | Touchpads de precisão mandam `WM_MOUSEWHEEL` com frações de dente (delta < 120), inclusive a **inércia** depois que o dedo sai. Cada mensagem contava como um dente inteiro. | **confirmada** (comportamento documentado do Windows) | Deltas somados; só 120 acumulados viram um dente; a sobra expira em 400 ms. |
| UEFI (pen-drive) | Z falso vindo do firmware. **Hipótese:** drivers USB de firmware (EDK2) põem o mouse no protocolo *boot*, em que só 3 bytes do relatório valem, e ainda assim leem o 4º como roda; mouses que mandam outra coisa ali geram Z contínuo. | **provável, não confirmada no PC do usuário** (o que se diz do EDK2 vem de memória do código-fonte, não foi conferido) | Filtro por dispositivo (abaixo). Confirmação: `FOLIO\DIAGNOSTICO.TXT` do PC afetado. |

`ResolutionZ = 0` não serve de critério: firmwares informam 0 também para mouses com roda de verdade.

### Filtro da roda no UEFI (por dispositivo)
- **Valor absurdo:** \|Z\| > 24 numa leitura é lixo.
- **Densidade:** o mesmo Z em relatórios densos, 16 em 250 ms ou 40 em 1,5 s, é relatório periódico de mouse defeituoso (~125 Hz). Uma roda real raramente passa de ~25 dentes/s. O bloqueio sai depois de 0,6 s sem relatório.
- **Movimento:** o mesmo Z em 8 relatórios seguidos que também trazem movimento indica que o byte não é roda. O bloqueio sai no primeiro relatório com Z = 0.
- **Rolar devagar e sem parar nunca bloqueia.** A primeira versão bloqueava quem rolasse um dente a cada 200 ms por mais de 1,5 s, e mantinha o bloqueio enquanto a pessoa continuasse. Corrigido e coberto por teste.
- Com lixo a 125 Hz, ainda passam ~16 leituras (~130 ms) antes do bloqueio. Não dá para saber antes que é lixo.

`FOLIO\DIAGNOSTICO.TXT` mostra, por dispositivo, as leituras de roda usadas e descartadas, o último Z bruto, quantos bloqueios houve por densidade e por movimento, e se a roda está bloqueada agora.

## 2. Rolagem automática

### Só liga quando o usuário pede
- Liga e desliga pelo **botão ▶/❚❚ visível** na barra de leitura, por um **clique do botão do meio** ou pela **paleta Ctrl+K** ("Rolagem automática da leitura"). A paleta é o caminho para PCs cujo firmware não tem driver de mouse; fora da leitura, ela abre a leitura no trecho do cursor e já liga.
- Nunca é restaurada: abrir a leitura (inclusive no boot, com `lendo=1`) começa sempre parado.
- **Esc** primeiro para o que estiver em movimento (arrasto do meio ou rolagem automática). **Esc nunca liga nada.** Na primeira versão, Esc durante um arrasto do meio *ligava* a rolagem; corrigido e coberto por teste.
- Para sozinha no fim do texto ("Fim do texto"). Num texto que cabe inteiro na tela, não liga e avisa.
- **Pausa só quando algo pede atenção:** diálogo ou paleta (janela modal), janela com o teclado (busca), arrasto da barra de rolagem. Janelas comuns (música, sumário, ajustes) **não** pausam. Na primeira versão, o player aberto deixava a rolagem ligada e parada; corrigido e coberto por teste.

### Barra de leitura
No centro da barra de status (não cobre o texto; aparece e some com as barras):

`‹ página anterior · ▶ rolagem automática · − 200 pal/min + · próxima página ›`

"pal/min" e não "ppm", que em português se lê "páginas por minuto". A roda do mouse sobre a barra muda a velocidade em vez de rolar. As teclas `+` e `−` também mudam. A velocidade é gravada 2 s depois do último ajuste.

No **modo zen**, as barras aparecem tocando a borda de cima e também a de baixo (na leitura), e ficam enquanto o mouse estiver sobre elas. Antes, a barra de leitura no zen sumia ao chegar perto dela.

### Velocidade: palavras por minuto, não pixels
Padrão 200, de 60 a 900 em passos de 20, gravado como `leitura_ppm=` no CONFIG.INI. A conversão para px/s usa a **densidade de palavras** da janela de leitura (60% da tela a partir da linha de leitura). Parágrafos densos passam mais devagar e títulos ou espaços mais depressa, de modo que o ritmo de leitura fica constante. A janela é larga e desliza continuamente, então não há trancos. Trecho quase vazio usa ao menos 35% da densidade média. **Tabelas contam as palavras das células** (antes passavam voando). O tempo restante na barra de status da leitura usa a mesma velocidade. Fora da leitura (F10, dica da contagem), a estimativa genérica continua a 200 pal/min.

### Suavidade
- **A página inteira anda em pixels inteiros.** Antes, cada linha arredondava sua posição por conta própria e, com altura de linha fracionária, as linhas (e fundos de código, barras de citação) pulavam de pixel em momentos diferentes: o texto "tremia" na rolagem lenta.
- Avanço por **tempo real** (`v · dt`), independente da taxa de quadros.
- **Partida em ~0,3 s** (suave) e **parada em ~0,1 s** (para quase onde foi pedido; medido: 1,2 px depois do Esc a 200 pal/min).
- Setas, roda e cliques nas margens funcionam durante a rolagem automática, que segue de onde estiver.
- Com o mouse parado por 2,5 s, barras e ponteiro somem. Isso só acontece se "ocultar barras" estiver ligado: a preferência do usuário vale também aqui.
- **Limites conhecidos:** sem subpixel vertical, o texto anda em passos de 1 px (~10 por segundo a 200 pal/min). O vídeo UEFI não sincroniza com a tela, então pode haver *tearing* (uma linha de corte) com a página inteira em movimento. Não há como evitar sem vsync.

### Energia
Desenhar 140 quadros por segundo para o texto mudar de pixel ~10 vezes gastava um núcleo da CPU inteiro. Agora a leitura pede o próximo quadro para quando o deslocamento arredondado for mudar (entre 7 e 50 ms; `ui.wake_ms`). Entre um quadro e outro, o laço dorme (no UEFI, `WaitForEvent`).

### Botão do meio
| Gesto | Efeito |
|---|---|
| Clique curto (< 0,45 s, sem arrastar) | liga/desliga a rolagem automática, igual ao botão ▶ |
| Segurar e arrastar | rola enquanto segura, como nos navegadores. Zona morta de 14 px; velocidade quase quadrática com a distância. O marcador ⌃•⌄ fica por cima das barras. |
| Soltar | para o arrasto (a rolagem automática, se ligada, continua) |

Só na faixa do texto: sobre as barras, o botão do meio não faz nada. **Rede de segurança:** qualquer tecla ou clique esquerdo encerra o arrasto, mesmo que o "soltar" do botão do meio se perca. No simulador, a captura do mouse só é solta quando nenhum botão está apertado, e perder o foco (Alt+Tab) solta todos. Antes, nos dois casos, um arrasto podia continuar rolando sozinho.

Por que não o modo "âncora" do Windows (clicar fixa um ponto)? Para **ler**, segurar o mouse imóvel a uma distância exata cansa, e um esbarrão muda a velocidade. A âncora fica para o arrasto (folhear), e o clique fica para a leitura em ritmo constante.

**UEFI:** o protocolo Simple Pointer só tem botões esquerdo e direito. **Esquerdo + direito juntos** fazem as vezes do meio (como o *Emulate3Buttons* do X11).

## 3. Testes (rodam no `build.ps1`)
- `tests/leitura.txt` (simulador), com asserções sobre a posição de rolagem (`assert_still`, `assert_moving`), o estado (`assert_auto`) e cliques nos botões da barra pelo nome (`barclick play`). Cobre:
  - nada rola sozinho;
  - clique do meio liga e Esc para;
  - o botão da barra liga e desliga;
  - o player aberto não pausa;
  - Esc durante o arrasto não liga;
  - o arrasto rola e uma tecla o encerra;
  - pela paleta Ctrl+K, só com o teclado, liga (saindo do editor) e desliga.
- `tests/efi_roda.txt` (o `BOOTX64.EFI` real no `efi_host`, com um mouse emulado que pode mandar Z falso: `zjunk`, `wheel`, `rclick`). Cobre:
  - o lixo é bloqueado sem levar a página ao início;
  - a roda de verdade volta depois;
  - rolar devagar e sem parar não bloqueia;
  - esquerdo+direito liga a rolagem e Esc para.
- **Controles negativos** (feitos uma vez, fora do build): com o filtro desligado, o teste da roda **falha**; com a regra antiga de 1,5 s, o teste de rolagem lenta **falha**. Os testes, portanto, detectam os defeitos que dizem detectar.

**Não verificado em hardware real:** o filtro da roda e o acorde esquerdo+direito. O `efi_host` emula o firmware, mas não um mouse USB de verdade.
