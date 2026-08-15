import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// https://vite.dev/config/
export default defineConfig({
  base: process.env.VITE_BASE ?? "/zmk-driver-paw3222-with-custom-studio-rpc/",
  plugins: [react()],
});
