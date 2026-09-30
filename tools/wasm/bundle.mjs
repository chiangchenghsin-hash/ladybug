// 手动 esbuild 打包(替代 build.mjs 中依赖 make 的部分)
import * as esbuild from "esbuild";
const ES_BUILD_CONFIG = {
  entryPoints: ["./build/index.js", "./build/lbug_wasm_worker.js"],
  bundle: true,
  format: "esm",
  external: ["fs", "path", "ws", "crypto", "worker_threads", "os", "util",
    "node:fs", "node:path", "node:crypto", "node:os", "node:util", "node:ws", "node:worker_threads"],
  outdir: "package",
  logLevel: "info",
  define: { importMeta: "import.meta" },
};
await esbuild.build(ES_BUILD_CONFIG);
console.log("ESBUILD DONE");
