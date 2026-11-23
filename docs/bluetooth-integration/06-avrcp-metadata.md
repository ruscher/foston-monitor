# AVRCP: o que o celular informa e o que o FOSTON controla

`bfbt-agent` lê `org.bluez.MediaPlayer1` e `MediaTransport1` e escreve em `/run/bigfoston-bt`:
`player=estado|título|artista|aparelho|álbum|duração|posição|instante|repeat|shuffle|gênero|faixa|total`
e `stream=idle|pending|active`. A posição vem com o instante da leitura (CLOCK_MONOTONIC) e o shell
a extrapola enquanto toca.

POCO X4 Pro 5G (app de música de streaming, 22/11/2026):

| Campo | Recebido |
|---|---|
| Título, artista, duração, posição, estado | sim |
| Álbum | sim (em singles é igual ao título: a Multimídia não repete) |
| Repeat, Shuffle | sim (estado); **mudar é recusado** pelo app (`org.bluez.Error.Failed`) |
| Gênero, número da faixa, total | vazios |
| ImgHandle (capa) | vazio (precisa do BIP/obexd) |
| Nome do app, lista de músicas (navegação AVRCP) | ausentes |
| Volume absoluto do transporte | 127 (o FOSTON usa o volume local) |

Comandos (FIFO `/run/bigfoston-bt.cmd`): `play`, `pause`, `next`, `prev`, `repeat off|singletrack|alltracks`,
`shuffle off|alltracks`. Testado: próxima trocou a faixa, pausa pausou.
