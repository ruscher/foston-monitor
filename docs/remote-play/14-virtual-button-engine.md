# Controle virtual de toque único (Virtual Multi-Button State Engine)

`src/ui/screens/scr_brp.c`, vista da sessão.

## Estados

| Conjunto | O que é |
|---|---|
| `v_held` | botões com o dedo em cima agora (modo normal) |
| `v_lock` | botões travados: ficam pressionados até um novo toque |
| analógico | `stick_x`, `stick_y` (−32767..32767) e `stick_locked` |
| controle físico | o estado mapeado de `gamepad_info(0)`, somado a cada 8 ms |

O PC recebe `v_held | v_lock | físico`, com os eixos do toque ou do controle físico. Os gatilhos
LT/RT viram 0 ou 255 no toque e são proporcionais no controle físico.

Assim chegam ao PC, ao mesmo tempo:
- direcional + A;
- correr + pular;
- LT + RT.

**Isso é entrada simultânea simulada, não multitouch.**

## Modos

- **Normal:** o botão vale enquanto o dedo está nele.
- **Travar** (botão amarelo na faixa de cima): cada toque liga ou desliga o botão, que fica com a
  borda amarela. Soltar o analógico fora do centro o deixa travado (anel amarelo).
- **Soltar tudo:** zera toque, travas e analógico e manda `RELEASE` ao motor.

## Analógico num ponto só

- **Zona morta de 15 % do raio**, para o tremor do resistivo.
- **Arrastar orienta.** Soltar volta ao centro, ou fica travado no modo Travar.
- O eixo Y vai invertido para o padrão do Moonlight (para cima é positivo).

## Nada fica preso no PC

| Quando | Como |
|---|---|
| Soltar tudo / Encerrar | `RELEASE` (e `STOP`) |
| Sair da tela | `RELEASE`, `STOP` e a entrada do motor fechada |
| O shell cair | o motor vê o fim da entrada, solta tudo e para a sessão |
| A conexão cair | o motor zera o estado; o shell apaga as travas |

## Pendente

Combos com nome, layouts (PlayStation, corrida, luta...), analógico direito na tela, tamanho e
transparência ajustáveis e perfil por jogo.
