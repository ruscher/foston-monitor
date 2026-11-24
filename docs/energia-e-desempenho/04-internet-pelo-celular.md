# Etapa 4 — Internet pelo celular (Bluetooth PAN)

Com "Tethering Bluetooth" ligado no celular, o FOSTON agora usa a internet dele.

## Como funciona (`src/bt/bfbt-agent.c`)

1. **Detecção:** o agente lê os UUIDs de cada aparelho (Device1). Quem anuncia NAP
   (`00001116-…`) pode dar internet.
2. **Conexão:** com o celular pareado e conectado, e fora da lista de desligados, o agente chama
   `org.bluez.Network1.Connect("nap")` (chamada assíncrona). Se falhar, tenta de novo a cada
   30 s.
3. **Interface:** o BlueZ cria a `bnep0` e o agente inicia `udhcpc -i bnep0 -f -R -t 8 -T 3 -A 10`.
4. **Rota e DNS:** o udhcpc roda com o script `/usr/sbin/bigfoston-pan-dhcp`. Ele chama o padrão
   (rota com métrica 0, que ganha da `usb0`, de métrica 200) e depois arruma o DNS:
   - o do celular vai para o **topo** de `/run/resolv.conf`. O musl só usa os três primeiros
     `nameserver`; antes, o do celular ficava em quarto e era ignorado;
   - a linha fica **sem** o comentário `# bnep0`, que o `nslookup` do busybox não entende
     ("bad address"). As linhas do celular ficam anotadas em `/run/resolv.bnep0`.
5. **Saída:** ao desconectar ou desligar, o agente para o DHCP e desconecta o PAN. A rota e as
   linhas anotadas saem junto.

A escolha de cada aparelho fica em `/data/bigfoston/bt-internet-desligada` (lista de endereços).
O padrão é ligado.

## Na tela (Ajustes > Bluetooth)

Na linha do celular aparecem:
- o estado:
  - "Internet pelo celular: conectada (10.124.78.88)";
  - "Internet pelo celular: ligue o Tethering Bluetooth no celular";
  - "Internet pelo celular: pegando endereço...";
  - "Internet pelo celular: desligada";
- um botão de **globo** que liga e desliga o uso da internet daquele aparelho (comando
  `internet ENDEREÇO 1|0` no FIFO do agente).

O estado vem do agente em `/run/bigfoston-bt`:
- linhas `pan=ENDEREÇO|quer|conectado|ip`;
- o shell lê em `btstat` os campos `nap`, `inet_want`, `inet_on` e `inet_ip`.

## Conferido no aparelho

POCO X4 Pro 5G com tethering Bluetooth:
- `bnep0` com 10.124.78.88;
- rota padrão pelo celular;
- DNS acrescentado;
- `ping 1.1.1.1` ok e HTTP ok.

A conexão continuou de pé durante os testes de TV e do plano de energia.

## "Conecta e pega IP, mas o app Internet não abre nada" (24/11/2026)

Investigado no aparelho, em camadas:

1. **Rede, DNS e HTTPS estavam certos.**
   - `ping`, `nslookup` e HTTP respondiam.
   - O `bfdl`, com a mesma libcurl e o mesmo OpenSSL do navegador, baixou
     `https://pt.wikipedia.org` (262 KB) pela `bnep0`.
2. **Modo PC.** Com "Navegação completa pelo PC" ligado (o padrão) e o cabo USB no PC com o
   `bfweb-pc.py`, as páginas vêm do Chromium do PC, pela internet do PC. Sem o PC, quem navega é
   o motor do FOSTON (NetSurf), pela internet do celular.
3. **O motor do FOSTON não abria nenhuma página HTTPS.**
   - HTTP abria.
   - Com o log do NetSurf (`bfweb -v`), cada conexão HTTPS levava ~17 s no handshake e terminava
     em erro 35 do curl: o servidor desiste de esperar.
4. **A causa:** o `bfphone` (telefone) girando a **91 % da CPU** desde o boot.
   - O motor roda com nice 10 e ficava só com as sobras. O handshake TLS precisa de ~2–4 s de
     CPU no ARM11.
   - A mesma disputa explica parte da CPU a 100 % na TV.

| Situação | `https://example.com` pelo motor |
|---|---|
| `bfphone` girando, motor com nice 10 | 17 s e erro 35 |
| `bfphone` girando, motor com nice 0 | 4,1 s |
| **`bfphone` corrigido, motor com nice 10** | **~2,5 s** |

Depois da correção, a Wikipédia abriu no app Internet pelo motor do FOSTON, pela internet do
celular.

### O que mudou no `bfphone`

- **Socket HCI em erro:** o laço só tratava `POLLIN`. Agora, em `POLLERR`, `POLLHUP` ou
  `POLLNVAL`, ele fecha o socket e reabre 2 s depois.
- **Guarda contra laço ocupado:** depois de mil voltas seguidas com o `poll` acordando na hora em
  menos de 2 s, ele registra no syslog os eventos de cada descritor (só números) e passa a dormir
  50 ms por volta.
- **Causa ainda não confirmada:** o processo travado já tinha sido substituído. Ele teve 3.593
  trocas de contexto voluntárias contra 510 mil forçadas, ou seja, o `poll` voltava na hora. Se
  acontecer de novo, o syslog mostra o descritor: procure `laço ocupado` com `grep` em
  `/var/log/messages`.

## Páginas abrindo "com muita dificuldade" (24/11/2026, segunda rodada)

Medido com um artigo da Wikipédia (294 KiB de HTML) pelo motor do FOSTON:

1. **O motor girava a 100 % da CPU.**
   - A libcurl 8.20 põe no `fd_set` o eventfd com que a thread do resolvedor de nomes a acorda,
     mas só o esvazia dentro de `curl_multi_poll`/`wait`. O NetSurf usa `select`.
   - Depois do primeiro sinal, o eventfd ficava sempre pronto: ~800 voltas por segundo sem nada a
     fazer.
   - Correção em `src/web/frontend/gui.c` (`drain_wakeups`): o que estiver pronto e não for
     socket nem os comandos é lido uma vez, como a curl faz no `curl_multi_poll`.
   - Resultado: CPU do motor em 76 s de 66 s para **11 s**.
2. **O tethering do celular parou de encaminhar.**
   - Com a `bnep0` "conectada", o celular não respondia nem ao ping no gateway nem ao DHCP ao
     reconectar.
   - Passavam só ~500 bytes/s; até o `bfdl` parava no meio do arquivo.
   - Antes, a mesma conexão tinha baixado 262 KB inteiros. Desligar e religar o "Tethering
     Bluetooth" no celular resolve.
   - Pendente: o agente detectar isso sozinho (gateway sem resposta) e avisar na tela.
3. **JavaScript:** agora sempre ligado no motor (o Google recusa sem ele). O interruptor saiu do
   menu. É o Duktape do NetSurf: páginas que dependem de JavaScript moderno podem continuar
   incompletas.

### Cache no cartão (pedido) — por que não foi feito

O motor roda como o usuário `bigfoston`, sem privilégios, e o cartão é vfat (dono e permissões
fixos para o root).

- Deixar o motor gravar no cartão daria a ele o cartão inteiro, inclusive os programas que o root
  executa (`BigFoston/tv/bftvd`, por exemplo). Uma página maliciosa que explorasse o motor viraria
  root.
- O caminho seguro seria um arquivo-imagem ext4 montado em loop. O kernel do FOSTON não tem nem
  loop nem ext4 (seria preciso recompilar e regravar o kernel).
- O NetSurf também foi compilado sem o armazenamento em disco (`NETSURF_FS_BACKING_STORE`).

Imagens por último: o NetSurf já desenha a página quando o HTML e o CSS chegam e as imagens entram
depois, uma a uma. O que impedia ver isso era o laço girando e o download parado.
