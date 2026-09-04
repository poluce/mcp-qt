// mcp_apps_server.js — MCP Apps Mock 服务器（A4 端到端验证）
// 支持 legacy Streamable HTTP (SSE)：
//   - initialize / tools/list / resources/read / tools/call
//   - tools/list 返回声明 _meta.ui.resourceUri 的 MCP Apps 工具
//   - GET /ui/mock/dashboard 返回 App HTML（供客户端 fetchUiResource）
// 启动：node mcp_apps_server.js [--port 9123]
const http = require('http');
const url = require('url');

const PORT = (() => {
  const i = process.argv.indexOf('--port');
  return i >= 0 ? parseInt(process.argv[i + 1], 10) : 9123;
})();
// --port 0 时 listen 随机端口，实际端口在启动后确定
let ACTUAL_PORT = PORT;

const APP_HTML = `<!DOCTYPE html>
<html>
<head><meta charset="utf-8"></head>
<body>
  <h2>MCP Apps Mock Dashboard</h2>
  <div id="status">ready</div>
  <script>
    window.addEventListener('DOMContentLoaded', function () {
      setTimeout(function () {
        if (window.mcpAppHost) {
          window.mcpAppHost.postMessage({ jsonrpc: '2.0', id: 1, method: 'ui/initialize',
            params: { appCapabilities: { tools: true, availableDisplayModes: ['inline'] } } });
        }
      }, 100);
    });
  </script>
</body>
</html>`;

const activeSessions = new Map();

function processMcpRequest(request) {
  if (typeof request !== 'object' || request === null) {
    return { jsonrpc: '2.0', id: null, error: { code: -32600, message: 'Invalid Request' } };
  }
  const { id, method, params, jsonrpc } = request;
  if (jsonrpc !== '2.0') {
    return { jsonrpc: '2.0', id: id !== undefined ? id : null, error: { code: -32600, message: 'Invalid jsonrpc' } };
  }
  if (id === undefined) return null; // 通知忽略
  const resp = { jsonrpc: '2.0', id };

  if (method === 'initialize') {
    resp.result = {
      protocolVersion: '2025-11-25',
      capabilities: { tools: { listChanged: false }, resources: { subscribe: false, listChanged: false } },
      serverInfo: { name: 'mcp-apps-mock', version: '1.0.0' }
    };
  } else if (method === 'shutdown') {
    resp.result = {};
    setTimeout(() => process.exit(0), 100);
  } else if (method === 'tools/list') {
    resp.result = {
      tools: [{
        name: 'show_dashboard',
        description: 'Render an interactive MCP Apps dashboard',
        inputSchema: {
          type: 'object',
          properties: { region: { type: 'string', description: 'deployment region' } },
          _meta: { ui: { resourceUri: `http://127.0.0.1:${ACTUAL_PORT}/ui/mock/dashboard`, visibility: ['model', 'app'] } }
        }
      }]
    };
  } else if (method === 'resources/read') {
    const uri = params && params.uri;
    if (uri === 'ui://mock/dashboard') {
      resp.result = { contents: [{ uri, mimeType: 'text/html;profile=mcp-app', text: APP_HTML }] };
    } else {
      resp.error = { code: -32602, message: `Resource not found: ${uri}` };
    }
  } else if (method === 'tools/call') {
    resp.result = { content: [{ type: 'text', text: 'dashboard rendered (mock)' }] };
    // MCP Apps 演示：额外返回 text/html;profile=mcp-app 嵌入式资源，宿主据此内嵌渲染
    resp.result.content.push({
      type: 'embeddedResource',
      resource: { text: APP_HTML, mimeType: 'text/html;profile=mcp-app' }
    });
  } else {
    resp.error = { code: -32601, message: `Method not found: ${method}` };
  }
  return resp;
}

function sendSse(session, message) {
  session.eventId++;
  session.res.write(`id: ${session.eventId}\n`);
  session.res.write(`data: ${JSON.stringify(message)}\n\n`);
}

const server = http.createServer((req, res) => {
  const parsedUrl = url.parse(req.url, true);
  const pathname = parsedUrl.pathname;

  // MCP Apps ui:// 资源 HTTP 端点（客户端 fetchUiResource 直取 HTML）
  if (pathname === '/ui/mock/dashboard') {
    res.writeHead(200, { 'Content-Type': 'text/html;profile=mcp-app; charset=utf-8' });
    res.end(APP_HTML);
    return;
  }

  if (pathname !== '/mcp') {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('Not Found');
    return;
  }

  if (req.method === 'GET') {
    const accept = req.headers['accept'] || '';
    if (!accept.includes('text/event-stream')) {
      res.writeHead(406, { 'Content-Type': 'text/plain' });
      res.end('Not Acceptable');
      return;
    }
    const sessionId = 's' + Math.random().toString(36).slice(2, 10);
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive',
      'MCP-Protocol-Version': '2025-11-25',
      'MCP-Session-Id': sessionId  // 客户端依赖此响应头获取 session
    });
    const session = { res, eventId: 1 };
    activeSessions.set(sessionId, session);
    console.error(`[Mock] GET SSE established session=${sessionId} total=${activeSessions.size}`);
    req.on('close', () => activeSessions.delete(sessionId));
    res.write(`id: 1\n`);
    res.write(`event: endpoint\n`);
    res.write(`data: /mcp?sessionId=${sessionId}\n\n`);
    return;
  }

  if (req.method === 'POST') {
    // 兼容两种 session 传递：query(sessionId) 与 MCP-Session-Id header
    const sessionId = parsedUrl.query.sessionId || req.headers['mcp-session-id'];
    const session = activeSessions.get(sessionId);
    let body = '';
    req.on('data', (c) => { body += c.toString(); });
    req.on('end', () => {
      let msg;
      try { msg = JSON.parse(body); } catch { msg = null; }
      const resp = msg ? processMcpRequest(msg) : { jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Parse error' } };
      if (session) {
        // 有 session：202 + SSE 下发响应（Streamable HTTP 标准）
        res.writeHead(202, { 'Content-Type': 'application/json' });
        res.end();
        if (resp) sendSse(session, resp);
      } else {
        // 无 session：宽容模式，直接返回 JSON 响应（客户端首次 POST 探测握手）
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(resp || { jsonrpc: '2.0', id: null, result: {} }));
      }
    });
    return;
  }

  res.writeHead(405, { 'Content-Type': 'text/plain' });
  res.end('Method Not Allowed');
});

server.listen(PORT, '127.0.0.1', () => {
  ACTUAL_PORT = server.address().port;
  console.error(`[MCP Apps Mock] LISTENING ${ACTUAL_PORT}`);
});
