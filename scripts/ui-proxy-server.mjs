import http from "node:http";
import { readFile } from "node:fs/promises";
import { extname } from "node:path";

const deviceBaseUrl = process.env.REBOOTER_DEVICE_BASE_URL || "http://192.168.1.48";
const port = Number(process.env.REBOOTER_UI_PROXY_PORT || 8787);

const contentTypes = {
  ".html": "text/html; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".ico": "image/x-icon",
};

const assetMap = new Map([
  ["/", new URL("../data/index.html", import.meta.url)],
  ["/index.html", new URL("../data/index.html", import.meta.url)],
  ["/app.js", new URL("../data/app.js", import.meta.url)],
  ["/style.css", new URL("../data/style.css", import.meta.url)],
]);

const server = http.createServer(async (req, res) => {
  try {
    const path = req.url || "/";
    if (assetMap.has(path)) {
      const fileUrl = assetMap.get(path);
      const body = await readFile(fileUrl);
      const type = contentTypes[extname(fileUrl.pathname)] || "application/octet-stream";
      res.writeHead(200, { "Content-Type": type, "Cache-Control": "no-store" });
      res.end(body);
      return;
    }

    if (path === "/favicon.ico") {
      res.writeHead(204, { "Cache-Control": "no-store" });
      res.end();
      return;
    }

    if (path.startsWith("/api/")) {
      const target = new URL(path, deviceBaseUrl);
      const headers = new Headers();
      for (const [key, value] of Object.entries(req.headers)) {
        if (typeof value === "string" && key.toLowerCase() !== "host") {
          headers.set(key, value);
        }
      }

      const chunks = [];
      for await (const chunk of req) {
        chunks.push(Buffer.from(chunk));
      }
      const body = chunks.length ? Buffer.concat(chunks) : undefined;

      const upstream = await fetch(target, {
        method: req.method,
        headers,
        body,
      });

      const upstreamBody = Buffer.from(await upstream.arrayBuffer());
      const responseHeaders = {};
      upstream.headers.forEach((value, key) => {
        if (key.toLowerCase() === "transfer-encoding") return;
        responseHeaders[key] = value;
      });
      responseHeaders["cache-control"] = "no-store";
      res.writeHead(upstream.status, responseHeaders);
      res.end(upstreamBody);
      return;
    }

    res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
    res.end(`Unknown path: ${path}`);
  } catch (error) {
    res.writeHead(500, { "Content-Type": "text/plain; charset=utf-8" });
    res.end(`Proxy error: ${error instanceof Error ? error.message : String(error)}`);
  }
});

server.listen(port, "127.0.0.1", () => {
  console.log(`Rebooter UI proxy serving on http://127.0.0.1:${port}`);
  console.log(`Proxying API traffic to ${deviceBaseUrl}`);
});
