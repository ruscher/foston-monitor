# Arquitetura

```
 bigfoston-shell (root, LVGL)                          cartão: BigFoston/web/
 ┌───────────────────────────────┐   pipe: comandos    ┌─────────────────────────────┐
 │ scr_web.c  barra, teclado,     │ ──────────────────▶ │ bfweb (usuário bigfoston,    │
 │            menus, toque        │ ◀────────────────── │  nice 10, RLIMIT_AS 88 MB)   │
 │ webproc.c  processo, memfd     │   pipe: eventos     │  NetSurf 3.11 + frontend     │
 │ webdata.c  favoritos/histórico │                     │  "bigfoston" (GPL-2.0)       │
 │ webdl.c    fila de downloads   │ ◀── memfd RGB565 ── │  página 800 x (conteúdo-52)  │
 └──────────────┬────────────────┘   (sem cópia)       └─────────────────────────────┘
                │ fork/exec, um por arquivo
                ▼
        bfdl (root, nice 10): libcurl → BigFoston/Downloads/<nome>.part → <nome>
```

## Por que dois processos

- **Isolamento**: HTML, imagens e JavaScript da internet são dados hostis. O `bfweb` roda
  como o usuário sem privilégios `bigfoston` (regra do `core/runner.c`), com cookies e
  dados em `/tmp` (RAM): não grava no cartão e não lê nada que o usuário comum não leia.
- **Memória**: limite de espaço de endereços (88 MB, bibliotecas incluídas). Uma página
  grande demais derruba só o motor; o shell mostra "a página usou memória demais" e
  oferece tentar de novo. O GPS, o telefone e o shell continuam.
- **Prioridade**: `nice 10` — o GPS e a interface vêm antes.
- **RAM de volta**: sair do app encerra o motor; a página reabre ao voltar.

## Tela da página

O `bfweb` desenha numa superfície libnsfb `ram` RGB565 e, depois de cada redesenho
completo, copia as linhas alteradas para a memória compartilhada (`memfd`, fd 3) e avisa
`R x0 y0 x1 y1`. O shell mostra essa memória num `lv_image` sem cópia e invalida só a
região. Rolagem: o motor move o que continua visível (`nsfb_plot_copy`) e renderiza só a
faixa nova. O shell junta os movimentos do dedo (até ~30 comandos por segundo) e faz a
inércia.

## Downloads

O motor não grava downloads: manda `DLREQ` (URL, nome, tamanho, tipo, origem, cookie da
sessão) e cancela a transferência. O shell higieniza o nome, escolhe um nome único e chama
o `bfdl`. Detalhes em [16](16-download-manager.md).

## Licenças

O frontend `src/web/frontend` é **GPL-2.0-only** (vai no mesmo binário do NetSurf, que é
GPL-2.0-only). O shell e o `bfdl` são programas separados (GPL-3.0-or-later) que conversam
com o motor só por pipe e memória compartilhada.
