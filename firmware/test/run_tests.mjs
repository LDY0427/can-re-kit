// 단위 시험 wasm 실행기(node:wasi). 사용: node --no-warnings test/run_tests.mjs <tests.wasm> <golden 폴더> [--update]
// golden 폴더를 wasm 안의 /golden으로 보인다(읽기, --update면 쓰기).
import { readFileSync } from 'node:fs';
import { WASI } from 'node:wasi';

const [wasmPath, goldenDir, ...rest] = process.argv.slice(2);
const wasi = new WASI({
  version: 'preview1',
  args: ['tests', '--golden', '/golden', ...rest],
  env: {},
  preopens: { '/golden': goldenDir },
  returnOnExit: true,
});
const mod = new WebAssembly.Module(readFileSync(wasmPath));
const inst = new WebAssembly.Instance(mod, wasi.getImportObject());
process.exitCode = wasi.start(inst);
