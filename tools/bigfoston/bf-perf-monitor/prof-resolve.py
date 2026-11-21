#!/usr/bin/env python3
"""prof-resolve.py BINÁRIO PROF.txt [N]: soma as amostras "tid pc" do perfilador do
shell (BIGFOSTON_PROF) por função, usando a tabela de símbolos (nm) do binário não
reduzido. Imprime as N funções com mais amostras (padrão 25) e o total por thread."""
import bisect, collections, subprocess, sys
binary, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
nm = sys.argv[4] if len(sys.argv) > 4 else "nm"
syms = []
for line in subprocess.run([nm, "-n", "-C", binary], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
by_fn, by_tid, total = collections.Counter(), collections.Counter(), 0
for line in open(prof):
    tid, pc = line.split()
    pc = int(pc, 16)
    i = bisect.bisect_right(addrs, pc) - 1
    by_fn[syms[i][1] if i >= 0 else "?"] += 1
    by_tid[tid] += 1
    total += 1
print(f"{total} amostras; por thread: " + ", ".join(f"{t}:{n}" for t, n in by_tid.most_common()))
for fn, n in by_fn.most_common(top):
    print(f"{100.0 * n / total:5.1f}%  {n:5d}  {fn}")
