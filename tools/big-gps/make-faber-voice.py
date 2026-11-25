#!/usr/bin/env python3
"""make-faber-voice.py - gera no PC o pacote de voz do Big GPS com a voz neural Piper
pt_BR-faber-medium (a única voz do Big GPS). O FOSTON só toca os áudios: o modelo não roda
no aparelho (docs/big-gps-2/22-piper-faber.md).

    make-faber-voice.py SAIDA.bgv [--nomes NOMES.txt] [--modelo pt_BR-faber-medium.onnx]

As frases fixas (manobras, distâncias, alertas) estão em frases(). Elas precisam bater com o
que o shell pede (src/ui/screens/scr_map.c: speak_parts e alert_check; biggps/bgnav.c:
bgn_voice_parts). NOMES: um nome de via por linha. Cada nome vira "na/no NOME" e
"para a/o NOME", como nas manobras. Sem o nome no pacote, o aparelho fala só a manobra.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MODELO = "/usr/share/piper-voices/pt/pt_BR/faber/medium/pt_BR-faber-medium.onnx"

# mesmas palavras de prep() em src/ui/biggps/bgnav.c: vias masculinas
MASC = ("Eixo", "Setor", "Acesso", "Anel", "Viaduto", "Túnel", "Balão", "Contorno", "Trevo", "Elevado", "Corredor",
        "Caminho", "Largo", "Beco", "Parque", "Boulevard", "Bulevar", "Ramal", "Retorno", "Complexo", "Conjunto",
        "Lote", "Jardim", "Passeio", "Calçadão")


def masc(nome):
    return any(nome == m or nome.startswith(m + " ") for m in MASC)


def distancias():
    """Os textos de spoken_dist() em scr_map.c."""
    out = [f"{m} metros" for m in range(50, 301, 50)] + [f"{m} metros" for m in range(400, 1001, 100)]
    out += ["1 quilômetro", "1 quilômetro e meio", "2 quilômetros"]
    out += [f"{k} quilômetros" for k in range(3, 31)]
    return out


def frases():
    f = []
    for d in distancias():
        f.append(f"Em {d}")
        f.append(f"Radar a {d}")
    verbos = ["Siga em frente", "Siga", "Continue em frente", "Vire levemente à direita", "Vire à direita",
              "Vire acentuadamente à direita", "Faça o retorno", "Vire acentuadamente à esquerda", "Vire à esquerda",
              "Vire levemente à esquerda", "Mantenha-se à direita", "Mantenha-se à esquerda",
              "Pegue o acesso à direita", "Pegue o acesso à esquerda", "Você chegou ao destino"]
    f += verbos
    f += [v[0].lower() + v[1:] for v in verbos]      # depois de "Em 300 metros," vem minúsculo (mesma chave)
    f += [f"Na rotatória, pegue a {n}ª saída" for n in range(1, 9)]
    f += ["e depois", "Recalculando a rota", "Radar à frente", "Lombada à frente", "Passagem de nível à frente",
          "Mantenha a atenção no trânsito"]
    f += [f"limite de {v} quilômetros por hora" for v in range(20, 130, 10)]
    return f


def nomes(path):
    out = []
    with open(path, encoding="utf-8") as fh:
        for linha in fh:
            n = linha.strip()
            if not n:
                continue
            a = "o" if masc(n) else "a"
            out += [f"n{a} {n}", f"para {a} {n}"]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("saida")
    ap.add_argument("--nomes")
    ap.add_argument("--modelo", default=MODELO)
    ap.add_argument("--piper", default=shutil.which("piper-tts") or shutil.which("piper") or "piper-tts")
    a = ap.parse_args()
    if not os.path.isfile(a.modelo):
        sys.exit(f"modelo não encontrado: {a.modelo} (pt_BR-faber-medium, rhasspy/piper-voices)")
    textos = frases() + (nomes(a.nomes) if a.nomes else [])
    vistos, lista = set(), []
    for t in textos:                                  # a chave é a frase normalizada (bgv_normalize)
        k = " ".join(t.split()).lower().rstrip(",.;:!? ")
        if k not in vistos:
            vistos.add(k)
            lista.append(t)
    pack = os.path.join(HERE, "builder", "bgvoice-pack")
    subprocess.run(["make", "-s", "-C", os.path.join(HERE, "builder"), "bgvoice-pack"], check=True)
    with tempfile.TemporaryDirectory(prefix="faber-") as tmp:
        linhas = []
        for i, t in enumerate(lista):
            # ponto final: a Faber termina a frase com entonação de fim; vírgula onde o aparelho
            # emenda outro trecho depois ("Em 300 metros," "vire à direita")
            fala = t + ("," if t.startswith(("Em ", "Radar a ")) or t == "e depois" else ".")
            linhas.append(json.dumps({"text": fala, "output_file": os.path.join(tmp, f"{i}.wav")}, ensure_ascii=False))
        print(f"gerando {len(lista)} frases com {os.path.basename(a.modelo)}...", flush=True)
        subprocess.run([a.piper, "-q", "-m", a.modelo, "--json-input", "--sentence_silence", "0"],
                       input="\n".join(linhas) + "\n", text=True, check=True, stdout=subprocess.DEVNULL)
        tsv = os.path.join(tmp, "lista.tsv")
        with open(tsv, "w", encoding="utf-8") as fh:
            for i, t in enumerate(lista):
                fh.write(f"{t}\t{os.path.join(tmp, f'{i}.wav')}\n")
        subprocess.run([pack, tsv, a.saida], check=True)


if __name__ == "__main__":
    main()
