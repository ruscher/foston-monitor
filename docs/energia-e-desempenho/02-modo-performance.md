# Etapa 2 — Modo performance para o sistema inteiro

O modo existia para vídeo no Internet e para controle de jogo
([../jogos-e-pacotes/07](../jogos-e-pacotes/07-latencia-e-modo-performance.md)). Agora ele
tem cinco motivos para ligar:

| Motivo | Liga quando | Pausa | Aviso |
|---|---|---|---|
| Manual | "Ativar agora" | o escolhido em "O que pausar" | janela com progresso |
| Vídeo | vídeo tocando há N s no Internet | o escolhido | janela com progresso |
| Controle | controle de jogo conectado | o escolhido | etiqueta breve |
| **TV** | a TV aberta ("Ligar sozinho quando abrir a TV", ligado de fábrica) | o escolhido | nenhum: a TV ocupa a tela |
| **Plano** | plano Economia ou Desempenho | só GPS e tarefas | nenhum |

Com a TV aberta, medido no aparelho:
- `bfgps` e `crond` param;
- o `bftvd` fica com nice −10 e a interface com −5;
- a etiqueta "Performance" não aparece por cima da imagem.

## Novidades no que ele faz

- **Papel de parede:** a opção "Tirar o papel de parede enquanto ligado" (ligada de fábrica)
  deixa o fundo liso enquanto houver um motivo forte (manual, vídeo, controle ou TV). Cada quadro
  redesenhado deixa de copiar a imagem de 800×480.
- **Bluetooth em uso:** qualquer dispositivo Bluetooth conectado (celular, teclado ou controle)
  segura o Bluetooth ligado, conferido a cada segundo pelo `btstat`.
  - **Antes:** o modo parava e religava o `bluetoothd` com o celular conectado (08:01, 08:11,
    08:33 e 08:43 no log). O celular caía e, ao voltar, pedia o código de novo
    ([05](05-pareamento-celular.md)).
- **Saída do controle:** quando o controle sai, o modo desliga na hora (`schedule(0)`), sem
  esperar o tempo de saída do vídeo.

## Prioridades (nice)

| Processo | Normal | Com o modo |
|---|---|---|
| `bigfoston-shell` | 0 | −5 |
| `bluetoothd` (com controle) | 0 | −10 |
| `bftvd` (TV) | **−10** (sempre) | −10 |
| tuner thread do `bftvd` | **−15** | −15 |

O `bftvd` fica acima da interface: a TV não perde a CPU para o shell nem para o Bluetooth. A
leitura do sintonizador fica acima de tudo, porque a FIFO do chip transborda se ela atrasa.
SCHED_FIFO continua fora, por política do projeto.

## Ajustes > Modo performance

Novos interruptores em "Automático":
- "Ligar sozinho quando abrir a TV".

Em "Enquanto ligado":
- "Tirar o papel de parede enquanto ligado".

O estado no alto diz o motivo: "Ligado: TV aberta" ou "Ligado pelo plano de energia".
