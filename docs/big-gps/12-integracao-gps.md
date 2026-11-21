# Big GPS — integração com o GPS do aparelho

O Big GPS **não** fala com o receptor: usa o serviço que já funciona ([docs/gps.md](../gps.md)).

| Item | Uso |
|---|---|
| `bfgps` + `S60gps` | motor MNL da MediaTek (`gps.dll` do próprio aparelho) rodando no Linux; não foi substituído nem alterado |
| `/run/bigfoston-gps` | estado reescrito a cada segundo (chave=valor): situação do fix, satélites, lat/lon, altitude, velocidade, rumo, HDOP, hora/data UTC (só com posição) — lido por `core/gpsstat.c` (`gps_read`) |
| `/run/gps-nmea` | NMEA cru (PTY) para outros programas; o Big GPS não precisa dele |

Fluxo na tela do mapa: `gps_read` a cada 1 s → posição viva (fix 2D/3D) ou "última posição
conhecida" (marcador cinza, salva em `biggps-last`) → `on_fix` → acompanhamento da rota
(`bgn_update`), câmera e marcador. Rumo só acima de 3 km/h (parado o rumo do GPS é ruído).
Sem posição, a rota parte da última posição conhecida e só a simulação fica liberada.

O relógio do sistema também usa a hora UTC do GPS quando não há NTP recente
([11-interface.md](11-interface.md)).
