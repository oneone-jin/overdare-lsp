// Files with this extension are inlined as raw text strings by esbuild's text loader
// (see esbuild.mjs) rather than resolved as a module at runtime.
declare module "*.d.luau" {
  const content: string;
  export default content;
}
