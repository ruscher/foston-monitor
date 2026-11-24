# Etapa 5 — Criar um app para o BigFoston

## 1. A pasta-fonte

```
apps/meu-jogo/
├── meu-jogo.desktop
├── icon.svg
├── .bfappignore        (opcional: o que não vai no pacote)
└── ...                 o resto do app
```

O nome da pasta é o id: `[a-z0-9._-]`, até 40 caracteres.

## 2. O `.desktop`

```ini
[Desktop Entry]
Type=Application
Name=Meu Jogo
Name[pt]=Meu Jogo
Comment=A short description
Comment[pt]=Uma descrição curta (aparece no "Instalar?")
Comment[es]=Una descripción corta
Categories=Game;ArcadeGame;
Icon=icon.svg
Exec=meu-jogo --tela-cheia
X-BigFoston-Color=#e5534b
```

| Chave | Uso no BigFoston |
|---|---|
| `Name`, `Name[pt]`, `Name[pt_BR]` | nome; vale o do idioma do sistema |
| `Comment` (também com idioma) | texto do "Instalar?" e do Run |
| `Categories` | o grupo: vale a primeira categoria principal ([tabela](01-categorias.md)) |
| `Icon` | SVG relativo à pasta; só a forma conta (silhueta) |
| `X-BigFoston-Color` | cor do quadrado do ícone; sem ela, vale a cor do grupo |
| `Exec` | o que roda; os códigos `%f %u...` são removidos |
| `Terminal=true` | abre o Terminal e digita o comando |
| `X-BigFoston-Screen` | abre uma tela do shell em vez do `Exec` (hoje: `sc-controller`) |
| `NoDisplay=true` / `Hidden=true` | não aparece |

## 3. O ícone

- Desenhe a **silhueta** em branco, num quadrado (o `viewBox` de 64×64 é bom).
- Para "furar" detalhes, use `fill-rule="evenodd"` no mesmo `path`, como os botões vazados
  do ícone do SC Controller.
- O shell rasteriza em 56×56, com 7% de margem, e pinta de branco sobre o degradê da cor.
- O rasterizador é o nanosvg, com ponto flutuante emulado no FOSTON. Prefira poucos caminhos
  e evite filtros, gradientes e texto. O ícone é desenhado uma vez só, mas pesado vai custar
  segundos.

## 4. Montar e instalar

```
python3 tools/bigfoston/bfapp.py build apps/meu-jogo
python3 tools/bigfoston/bfapp.py install pkg/apps/meu-jogo.bfapp
```

Também dá para copiar o `.bfapp` para o cartão e tocar nele em Arquivos.

## 5. Controles no app

Um app nativo que use o controle pode ler o estado padrão:
- `gamepad_info()`: botões A, B... e eixos já mapeados;
- `gamepad_capture(true, obs)`: o controle deixa de mexer na interface enquanto o app quiser
  os botões; `obs` recebe os eventos crus.

Um app pode trazer seu próprio `gamecontrollerdb.txt` na pasta: o shell o lê junto com os
outros (etapa 3).
