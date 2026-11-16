# FOLIO 4 — estudo de interface e funções

*12/11/2026 · Capturas em `build\estudo\antes` e `build\estudo\depois`, em 1920×1080.*

## Princípios adotados

1. **O texto é a interface.** Tudo o que não é texto precisa justificar a presença na tela.
2. **Visível quando chamado, invisível quando se escreve.** As barras aparecem com o mouse e somem ao digitar. Não há relógio, ícone ou número piscando no campo de visão.
3. **Uma casa para cada coisa.** O menu reúne as ações de *documento*. A barra superior guarda só os *modos de escrita*. Ctrl+K encontra tudo.
4. **O que o escritor vê é o que o leitor recebe.** O editor, o modo leitura e as exportações tratam o texto do mesmo jeito.
5. **Nunca perder texto.** Salvar em segundo plano, guardar versões e recuperar rascunhos, sem pedir nada ao usuário.

## Diagnóstico (antes)

| # | Problema | Gravidade |
|---|---|---|
| 1 | Clicar em qualquer botão da barra também movia o cursor do texto (o clique atravessava a barra até o editor) | **bug** |
| 2 | Uma linha nova no editor virava um parágrafo, mas a leitura e as exportações (DOCX, EPUB…) fundiam as linhas num parágrafo só | **bug de coerência** |
| 3 | 15 ícones na barra superior, metade deles de arquivo (abrir, salvar, exportar) e de ferramentas raras | alta |
| 4 | Barras como blocos sólidos com borda, cortando a folha de papel no topo e no rodapé | alta |
| 5 | O título mostrava "Sem título" mesmo com `# A casa do rio` na primeira linha | média |
| 6 | Barra inferior densa: palavras, sessão, leitura e modos, tudo ao mesmo tempo | média |
| 7 | Avisos (toasts) empilhavam até quatro mensagens | média |
| 8 | O anel de meta era desenhado com segmentos (serrilhado) e não alinhava com o anel de fundo | baixa |
| 9 | A janela de metas tinha espaço vazio e não permitia escolher a duração do sprint | baixa |
| 10 | A paleta de comandos tinha altura fixa, com área vazia | baixa |
| 11 | A meta só contava a sessão atual e zerava ao trocar de documento ou reiniciar | QoL |
| 12 | Não havia histórico de versões nem lista de documentos recentes | QoL |

## O que mudou

**Barras**
- **Barra superior:** à esquerda fica só o menu. No centro, o título derivado do texto (ou do nome do arquivo), com um ponto que indica alteração não salva ou um ✓ discreto quando está salvo. À direita ficam apenas os modos de escrita (foco, máquina de escrever, leitura), mais música (só se houver MP3), aparência e comandos. Clicar no título abre o gerenciador de arquivos.
- **Barras dissolvidas:** as duas barras viraram degradês que se fundem à cor da mesa e, sobre a folha, à cor do papel. A folha fica contínua de cima a baixo.
- **Barra inferior:** só o anel de meta e "N palavras" à esquerda, o relógio à direita e a música no centro quando há faixa tocando. Com texto selecionado, mostra "X de N palavras". Passar o mouse mostra as palavras de hoje e o tempo de leitura; clicar abre as metas.

**Menu e janelas**
- **Menu agrupado:** Documento, Recentes (até 4), Escrita e Preferências, mais Desligar.
- **Metas:** a janela conta as **palavras de hoje** (somando sessões e documentos, guardado no `CONFIG.INI`). Tem anel suave, os números do documento e da sessão, escolha rápida do sprint (15/25/45/60 min) e altura ajustada ao conteúdo.
- **Avisos:** um por vez (o novo substitui o anterior).
- **Paleta de comandos:** altura sob medida e mensagem quando nenhum comando é encontrado.

**Texto e proteção**
- **Cada linha é um parágrafo** no editor, na leitura e em todas as exportações.
- **Recuo na primeira linha** (Aparência → Tipografia), como em livro. Na leitura, o primeiro parágrafo após um título fica sem recuo, seguindo a convenção editorial.
- **Versões automáticas:** antes de sobrescrever um texto, a versão anterior é copiada para `FOLIO\VERSOES\Nome · AAAA-MM-DD HHhMM.md`. Isso acontece no máximo uma vez a cada 10 minutos por documento, e ficam guardadas as 30 mais recentes. A pasta é visível em qualquer computador.
- **Correção de clique:** clicar nas barras não mexe mais no texto.

## Recomendações seguintes (não implementadas)

- **Desenho parcial:** redesenhar só o cursor quando ele pisca, para economizar bateria em notebooks.
- **"Retomar de onde parei" por documento**, não só para o último arquivo aberto.
- **Navegação por capítulos** (Ctrl+PgUp/PgDn salta entre títulos `#`).
- **Contagem de palavras por capítulo** no Sumário.
- **Modo "manuscrito"** para exportar em DOCX/ODT no formato padrão de editoras: Times 12, espaço duplo, recuo e numeração de páginas.
- **Hifenização PT-BR** e **justificação** opcionais no modo leitura.
