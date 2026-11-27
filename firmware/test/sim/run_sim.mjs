// 가상 펌웨어 실행기. 사용: node --no-warnings test/sim/run_sim.mjs [sim.wasm 경로]
//   표준 입력 = 명령 줄, 표준 출력 = 실제 보드 시리얼과 같은 줄. 표준 입력이 닫히면 끝난다.
// 구조: 주 스레드가 표준 입력을 받아 공유 메모리 링에 넣고, 작업 스레드가 wasm을 돌린다.
//   wasm은 host.getc(대기 ms)로 막힌 채 기다릴 수 있고(Atomics.wait), host.avail로 막지 않고 물을 수 있다.
//   출력은 작업 스레드에서 fs.writeSync(1)로 줄마다 바로 쓴다(버퍼에 묵히지 않는다).
import { readFileSync, writeSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { WASI } from 'node:wasi';
import { Worker, isMainThread, workerData } from 'node:worker_threads';

const RING = 1 << 20; // 표준 입력 링 1 MiB
const W = 0, R = 1, EOF = 2; // Int32 제어 칸: 쓴 위치, 읽은 위치, 입력 닫힘

if (isMainThread) {
  const here = path.dirname(fileURLToPath(import.meta.url));
  const wasmPath = process.argv[2] || path.join(here, '..', 'build', 'sim.wasm');
  const sab = new SharedArrayBuffer(16 + RING);
  const ctl = new Int32Array(sab, 0, 4);
  const buf = new Uint8Array(sab, 16, RING);
  const pending = [];
  let flushing = false;
  const pump = () => {
    while (pending.length) {
      const chunk = pending[0];
      const w = Atomics.load(ctl, W), r = Atomics.load(ctl, R);
      const free = RING - (w - r);
      if (free <= 0) { setTimeout(pump, 2); return; }
      const k = Math.min(free, chunk.length);
      for (let i = 0; i < k; i++) buf[(w + i) % RING] = chunk[i];
      Atomics.store(ctl, W, w + k);
      Atomics.notify(ctl, W);
      if (k < chunk.length) pending[0] = chunk.subarray(k); else pending.shift();
    }
    if (flushing) { Atomics.store(ctl, EOF, 1); Atomics.notify(ctl, W); }
  };
  process.stdin.on('data', (c) => { pending.push(c); pump(); });
  process.stdin.on('end', () => { flushing = true; pump(); });
  const worker = new Worker(fileURLToPath(import.meta.url), { workerData: { sab, wasmPath } });
  worker.on('error', (e) => { process.stderr.write(`run_sim: ${e.stack || e}\n`); process.exit(2); });
  worker.on('exit', (code) => process.exit(code));
} else {
  const { sab, wasmPath } = workerData;
  const ctl = new Int32Array(sab, 0, 4);
  const buf = new Uint8Array(sab, 16, RING);
  let mem = null;
  const avail = () => Atomics.load(ctl, W) - Atomics.load(ctl, R);
  const host = {
    getc(timeoutMs) {
      for (let tries = 0; tries < 2; tries++) {
        if (avail() > 0) {
          const r = Atomics.load(ctl, R);
          const c = buf[r % RING];
          Atomics.store(ctl, R, r + 1);
          return c;
        }
        if (Atomics.load(ctl, EOF)) return -2;
        if (tries === 0) Atomics.wait(ctl, W, Atomics.load(ctl, W), Math.max(0, timeoutMs));
      }
      return Atomics.load(ctl, EOF) && avail() <= 0 ? -2 : -1;
    },
    avail() { return avail() > 0 ? 1 : 0; },
    drain() { Atomics.store(ctl, R, Atomics.load(ctl, W)); },
    write(p, n) {
      const bytes = new Uint8Array(mem.buffer, p, n);
      let off = 0;
      while (off < n) {
        try { off += writeSync(1, bytes, off, n - off); } catch (e) { if (e.code !== 'EAGAIN') throw e; }
      }
    },
  };
  const wasi = new WASI({ version: 'preview1', args: ['sim'], env: {}, returnOnExit: true });
  const mod = new WebAssembly.Module(readFileSync(wasmPath));
  const inst = new WebAssembly.Instance(mod, { ...wasi.getImportObject(), host });
  mem = inst.exports.memory;
  process.exit(wasi.start(inst));
}
