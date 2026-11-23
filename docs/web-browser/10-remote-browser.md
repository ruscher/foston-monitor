# Modo completo — o Chrome do PC navega, o FOSTON mostra

O FOSTON só tem internet através do PC (rede USB). O modo completo aproveita isso: um
Chrome/Chromium/Brave sem janela no PC renderiza a página do tamanho exato da área do app
Internet e o FOSTON mostra, toca e digita. Qualquer site que funciona no Chrome funciona
no aparelho: JavaScript moderno, CSS atual, logins, YouTube (imagem).

O motor local (NetSurf) continua sendo usado quando o PC não está ligado.

## Ligar

No PC (o mesmo que dá internet ao FOSTON):

```sh
tools/bigfoston/bfweb-pc.py            # espera o FOSTON e reconecta sozinho
```

Requer Python 3, Pillow, numpy e um Chrome, Chromium ou Brave (os scripts de atalho do
BigLinux em `/usr/local/bin` são pulados quando não apontam para um navegador instalado).
Para iniciar com a sessão: `tools/bigfoston/bigfoston-web-pc.desktop` (instalado pelo
pacote em `/etc/xdg/autostart`, ou copiado para `~/.config/autostart` com o `Exec` apontando
para o script).

No FOSTON: abra **Internet**. Na primeira vez aparece "Navegar pelo PC?" — **Permitir**. O
selo verde **PC** na barra indica o modo completo. Menu → "Navegação completa pelo PC"
liga/desliga.

## Como funciona

| Parte | O quê |
|---|---|
| Conexão | o PC se liga ao FOSTON em `192.168.7.2:7071` (só a rede USB): nenhuma porta aberta no PC, funciona até com o firewall do PC fechado |
| Autorização | chave aleatória do PC (`~/.config/bigfoston-web/key`); o FOSTON pergunta uma vez e guarda em `<dados>/web/pcs.tsv` |
| Navegador | Chrome headless com perfil próprio (`~/.local/share/bigfoston-web/chrome`, separado do seu Chrome), som mudo, user-agent normal do Chrome |
| Imagem | screencast PNG do Chrome (só quando a página muda) → RGB565 → blocos de 32×32 que mudaram, zlib → o FOSTON aplica e confirma (`Q`); um quadro por vez e no máximo ~12 por segundo |
| Comandos | os mesmos do motor local (`GO`, `CLICK`, `SCROLL`, `KEY`, `TEXT`, `LONG`...), viram eventos do Chrome (DevTools Protocol por pipe) |
| Teclado | um script na página avisa quando um campo de texto ganha o foco: o teclado do FOSTON abre sozinho |
| Título | o mesmo script avisa as trocas de `document.title` (o Chrome não as informa pelo `Target.targetInfoChanged`); páginas que trocam de endereço sem recarregar (vídeos do YouTube) entram no histórico com o título certo |
| Reconexão | ao religar (FOSTON reiniciado, app reaberto), a sessão anterior do Chrome é desanexada; mensagens de outra sessão são descartadas |
| Janelas novas | abrem na mesma aba |
| Downloads | o Chrome baixa (com a sessão dele) e o arquivo vai pelo cabo, em blocos de 128 KB confirmados, para `BigFoston/Downloads/<nome>.part` → renomeado ao conferir o tamanho |
| Fim | o FOSTON fecha o app → o Chrome é encerrado 2 minutos depois (a RAM do PC volta); Ctrl+C, `kill` ou fim da sessão do PC encerram o Chrome junto com o script |

Protocolo: tipo (1 byte) + tamanho (u32) + conteúdo; ver `src/ui/core/webremote.c`.

## Testado no FOSTON (23/11/2026)

biglinux.com.br (site completo, fundo animado), g1.globo.com, gov.br, DuckDuckGo completo
(toque no campo → teclado do FOSTON abriu sozinho → "distribuição biglinux" com acento →
Enter → resultados), YouTube (busca e vídeo tocando, imagem sem som), download de 9 MB pelo
PC (13 s, SHA-512 conferido no cartão). Números em [26](26-benchmarks.md).
