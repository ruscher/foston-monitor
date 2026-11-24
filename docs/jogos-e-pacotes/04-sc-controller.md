# Etapa 4 — O app SC Controller

Grupo Jogos, pacote `sc-controller.bfapp` (15,8 KB; 203 KB instalado). A tela é
`src/ui/screens/scr_scc.c`, aberta pelo `.desktop` com `X-BigFoston-Screen=sc-controller`.

As três abas ficam num seletor em pílula no alto, com folga em cima; a aba escolhida tem o
degradê roxo. O conteúdo ocupa a altura real da tela: 360 px com a barra inferior, 432 px no
Foston Glass. O controle desenhado cresce até onde cabe (`pad_width`).

## O desenho do controle (`src/ui/widgets/padview.c`)

O mesmo desenho serve às três abas:
- **silhueta:** um SVG em texto (corpo e pegadas, como um DualShock visto de frente, 400×250)
  rasterizado uma vez por tamanho como máscara A8 (`svg_render_mask_str`); fica guardado;
- **acabamento:** sombra deslocada, corpo cinza-azulado e contorno roxo;
- **peças por cima,** nas proporções de um DualShock: gatilhos como barras que enchem, ombros
  L1/R1 (LB/RB), direcional, botões de frente (△ ○ ✕ □ desenhados com linhas no PlayStation;
  A B X Y nas cores do Xbox nos outros), touchpad, Share/Options, PS/Guia e analógicos com o
  ponto que anda;
- `padview_state` acende as peças: cor cheia, brilho e o traço que contrasta;
- `padview_focus` faz um anel amarelo pulsar em volta do controle da vez (no analógico, também
  uma seta: → para a direita, ↓ para baixo);
- `padview_done` deixa em verde o que já foi ensinado.

Medido no aparelho: a primeira abertura rasteriza a silhueta (uma vez por tamanho); depois
abrir as abas é imediato.

## Controles

Um cartão por controle conectado:
- a miniatura do controle (`padview` de 160 px) e o nome;
- selos arredondados: Bluetooth com o endereço (ou USB), **bateria** com o ícone do nível
  (lida de `/sys/class/power_supply/ps-controller-battery-<endereço>`, que o `hid-playstation`
  publica) e o **mapa** com a origem: "banco do SC Controller", "embutido no BigFoston",
  "registrado por você" ou, em amarelo, "padrão do Linux: registre se algum botão estiver
  trocado";
- o GUID;
- os botões **Testar** e **Registrar**.

Sem controle, o cartão diz como ligar ("Já pareado? Aperte PS") ou parear, e tem o botão
Abrir Bluetooth. No rodapé, um selo com o tamanho do banco: "746 modelos no banco · N
registrados por você". A lista se refaz sozinha quando um controle chega ou sai: o timer
confere a cada 400 ms.

## Testar

À esquerda, o controle desenhado e centralizado; à direita, um cartão de leituras:
- o nome e a bateria;
- os dois analógicos em números;
- os gatilhos em barras;
- o último evento cru, por exemplo `KEY 0x131 b1 = 1`, com o código do evdev e o número do
  SDL (para comparar com o banco);
- a contagem de eventos;
- a dica "Segure B (Bola) por 2 s para sair do teste" e uma barra que enche enquanto B está
  segurado.

Enquanto o testador está aberto, o controle **não mexe na interface** (`gamepad_capture`):
o B não volta e o cursor some. O timer roda a 30 quadros/s só nesta aba.

## Registrar (o "Device registration" do sc-controller)

À esquerda, o controle com o botão da vez pulsando e o que já foi ensinado em verde; à
direita, o cartão com a etapa ("3 de 21" e a barra), a instrução em letra grande, "Solte tudo"
ou o aviso de botão repetido, e Voltar / Pular. No fim, ✓, "Pronto: N controles ensinados" e
Voltar / Salvar na mesma linha, com o controle todo em verde.

Para controle sem mapa ou com botão trocado.

1. "Ensine ao BigFoston cada botão do seu controle" e o botão **Começar**. Se já houver
   registro, aparece também **Esquecer registro**.
2. São **21 etapas**, com barra de progresso, "N de 21", **Voltar** e **Pular** (para
   controle sem aquele botão):
   - A, B, X, Y;
   - LB, RB, LT, RT;
   - Back, Start, Guia;
   - L3, R3;
   - as 4 direções do direcional;
   - analógico esquerdo para a direita e para baixo;
   - analógico direito para a direita e para baixo.
3. **Como cada etapa reconhece**:
   - **botão**: vira `bN`;
   - **direcional**: vira `hN.máscara`;
   - **eixo**: conta a partir de 60% de movimento, medido a partir do repouso de quando a etapa
     começou;
   - **gatilho**: se repousa no mínimo, vira eixo inteiro (`aN`); se repousa no meio, vira
     metade (`+aN`/`-aN`);
   - **analógico**: o sinal do movimento decide `~` (invertido);
   - **direcional analógico**: vira metade de eixo.

   Entre uma etapa e outra é preciso **soltar tudo**. Uma fonte que já está com outro controle
   é recusada, com aviso.
4. **Pronto: N controles ensinados** e o botão **Salvar**. A linha vai para
   `<dados>/controles/gamecontrollerdb.txt`, no formato do SDL, trocando a anterior do mesmo
   GUID. Ela vale na hora (`gamepad_remap`) e o app volta ao testador.

Conferido: registrar o DualShock 4 (sem driver) pelas 21 etapas gerou **exatamente** a linha
do SDL GameControllerDB (o touchpad não é perguntado), no simulador e no aparelho.

## Uso de recursos (aparelho)

| Medida | Valor |
|---|---|
| CPU do shell parado na aba Controles | 3,8% (outras telas: 3,1–3,2%) |
| RSS do shell com o app e um controle | 4,7 MB |
| Achar o mapa ao conectar | lê no máximo 1 + 8 bancos (~200 KB), uma vez por controle |
