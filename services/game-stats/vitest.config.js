import { cloudflareTest, readD1Migrations } from "@cloudflare/vitest-pool-workers";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [cloudflareTest({
    wrangler: { configPath: "./wrangler.toml" },
    miniflare: { bindings: { TEST_MIGRATIONS: await readD1Migrations("./migrations") } },
  })],
  test: { fileParallelism: false },
});
