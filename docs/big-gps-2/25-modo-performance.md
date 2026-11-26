# Desempenho sem desligar nada essencial

## Perfis do mapa (Ajustes > Desempenho)

| Perfil pedido | No BigFoston | O que muda no Big GPS |
|---|---|---|
| Automático | Automático (recomendado) | Econômico ao navegar ou se os quadros passarem de 300 ms em média; equilibrado no resto. 3D liga se o botão estiver em 3D, e recua sozinho com lentidão sustentada |
| Desempenho | Econômico | Vias e rótulos; sem edifícios e caminhos até bem perto; **sempre 2D** |
| Equilibrado | Equilibrado | Edifícios e caminhos a partir do zoom de bairro |
| Qualidade | Detalhado | Tudo o que o mapa tem |

O que cai primeiro quando falta fôlego é só trabalho gráfico:
1. detalhe das camadas;
2. a inclinação 3D;
3. o giro entre quadros, que nunca é feito (medido e descartado, [04](04-arquitetura-renderizacao.md)).

O movimento do carro (20 Hz, [05](05-gps-interpolacao.md)) e a voz não dependem do perfil.

## O modo Performance do sistema não pausa nada que a navegação use

O modo Performance do BigFoston (vídeo no app Internet ou controle de jogo) pausa serviços
(GPS, Bluetooth, telefone, cron). **Com o Big GPS aberto, o mapa segura o GPS, o Bluetooth e o
telefone** (`app_needs` em `core/perfui.c`). Antes segurava só o GPS: um celular pareado mas
desconectado naquele instante podia ficar sem Bluetooth e sem chamadas. O Bluetooth também
nunca pausa com qualquer aparelho conectado (já era assim).

Nenhum caminho do Big GPS desliga Bluetooth, GPS, telefone ou rede para ganhar quadros.
