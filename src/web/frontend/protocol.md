# Protocolo do bfweb

Linhas UTF-8 terminadas em `\n`, uma por comando/evento. Sem `\n` dentro de campos;
campos com espaço vêm por último ou separados por tab.

## Linha de comando

`bfweb [--fd N | --shm ARQ] -w L -h A --data DIR --res DIR [--js] [--no-images] [url]`

A memória compartilhada (fd ou arquivo) tem `L*A*2` bytes, RGB565, linha a linha.

## Comandos (stdin)

| Comando | Efeito |
|---|---|
| `GO url` | navega |
| `BACK`, `FWD`, `RELOAD`, `STOP` | histórico da janela, recarregar, parar |
| `CLICK x y` | toque (coordenadas da janela) |
| `LONG x y` | o que há no ponto → evento `FEAT` |
| `SCROLL dx dy` | rola (a página ou um quadro interno sob o centro) |
| `SCROLLTO x y` | posição absoluta |
| `KEY nome` | enter, backspace, delete, left, right, up, down, tab, shifttab, escape, home, end, pageup, pagedown, selectall, copy, paste |
| `TEXT texto` | digita (UTF-8) |
| `ZOOM pct` | 30..400 |
| `DOWNLOAD url` | baixa o link (com origem e cookies) → `DLREQ` |
| `CLOSEWIN` | fecha a janela aberta por um link e volta à anterior |
| `REDRAW` | redesenha tudo |
| `QUIT` | salva cookies e sai |

## Eventos (stdout)

| Evento | Significado |
|---|---|
| `READY L A` | pronto para comandos |
| `R x0 y0 x1 y1` | região pronta na memória compartilhada |
| `URL u`, `TITLE t`, `STATUS s` | da janela ativa |
| `LOAD 1` / `LOAD 0` | começou / terminou de carregar |
| `NAV voltar avançar` | 0/1 |
| `SEC n` | estado de segurança (6 = HTTPS ok, 3 = HTTP, 4/5 = HTTPS com problemas) |
| `SCROLL x y largura altura` | posição e tamanho da página |
| `CARET 1 x y h [new]` / `CARET 0` | campo de texto com cursor (o shell abre o teclado) |
| `FEAT x y\tlink\tobjeto` | resposta do `LONG` |
| `DLREQ url\tnome\ttamanho\tmime\torigem\tcookie` | pedido de download (o motor não grava) |
| `LAUNCH url` | esquema que o motor não abre (mailto:, tel:...) |
| `ZOOM pct`, `WINDOWS n`, `WARN texto` | |

`BFWEB_PROF=arquivo` grava um perfil de CPU (amostra a cada 2 ms) ao sair;
`tools/bigfoston/web-prof.py` resume por função.
