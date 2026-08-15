import * as esbuild from "esbuild";

const production = process.argv.includes("--production");
const watch = process.argv.includes("--watch");

const ctx = await esbuild.context({
  entryPoints: ["src/extension.ts"],
  bundle: true,
  format: "cjs",
  minify: production,
  sourcemap: !production,
  sourcesContent: false,
  platform: "node",
  outfile: "dist/extension.js",
  external: ["vscode"],
  logLevel: "warning",
  // OVERDARE's globalTypes.d.luau is inlined as a string so the extension never needs to
  // resolve a path to scripts/ at runtime - works identically in dev and in a packaged vsix.
  loader: { ".luau": "text" },
  plugins: [
    {
      name: "esbuild-problem-matcher",
      setup(build) {
        build.onStart(() => {
          console.log("[watch] build started");
        });
        build.onEnd((result) => {
          for (const { text, location } of result.errors) {
            console.error(
              `✘ [ERROR] ${text}`,
              location
                ? `${location.file}:${location.line}:${location.column}:`
                : "",
            );
          }
          console.log("[watch] build finished");
        });
      },
    },
  ],
});

if (watch) {
  await ctx.watch();
} else {
  await ctx.rebuild();
  await ctx.dispose();
}
