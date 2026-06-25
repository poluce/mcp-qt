// test_mcp_server.js
// 这是一个支持 Stdio 和 HTTP/SSE (Streamable HTTP) 双重协议的 Mock MCP 服务端，用于集成测试。
const http = require('http');
const url = require('url');

const isHttp = process.argv.includes('--http');

// 支持的工具列表
const ALL_TOOLS = [
  {
    name: "calculate_add",
    description: "计算两个数字的和",
    inputSchema: {
      type: "object",
      properties: {
        a: { type: "number", description: "第一个数字" },
        b: { type: "number", description: "第二个数字" }
      },
      required: ["a", "b"]
    }
  },
  {
    name: "get_system_time",
    description: "获取系统当前时间（无参数测试工具）",
    inputSchema: {
      type: "object",
      properties: {}
    }
  },
  {
    name: "get_system_info",
    description: "获取系统版本及当前状态",
    inputSchema: {
      type: "object",
      properties: {}
    }
  },
  {
    name: "trigger_exception",
    description: "触发工具内部异常的测试工具",
    inputSchema: {
      type: "object",
      properties: {}
    }
  }
];

// 支持的资源列表
const RESOURCES = [
  {
    uri: "file:///logs/system.log",
    name: "系统日志文件",
    mimeType: "text/plain",
    description: "保存系统运行时的核心状态日志"
  },
  {
    uri: "file:///configs/app.json",
    name: "应用配置文件",
    mimeType: "application/json",
    description: "本地调试应用的配置文件"
  }
];

// 支持的提示词模版
const PROMPTS = [
  {
    name: "code_review",
    description: "对指定的 C++ 代码进行静态分析和代码审查",
    arguments: [
      {
        name: "code",
        description: "待审查的代码文本内容",
        required: true
      }
    ]
  }
];

// 核心 JSON-RPC 处理逻辑，由 Stdio 与 HTTP/SSE 共用
function processMcpRequest(request) {
  const { id, method, params } = request;
  if (id === undefined) return null; // 忽略通知

  let response = {
    jsonrpc: "2.0",
    id: id
  };

  if (method === 'initialize') {
    response.result = {
      protocolVersion: "2025-11-25",
      capabilities: {
        tools: { listChanged: false },
        resources: { subscribe: false },
        prompts: { listChanged: false }
      },
      serverInfo: {
        name: "mock-test-server",
        version: "1.1.0"
      }
    };
  } else if (method === 'shutdown') {
    response.result = {};
    // 延迟 100ms 退出进程
    setTimeout(() => {
      process.exit(0);
    }, 100);
  } else if (method === 'tools/list') {
    const cursor = params ? params.cursor : undefined;
    if (!cursor) {
      // 第一页返回前两个工具，且给出 nextCursor
      response.result = {
        tools: ALL_TOOLS.slice(0, 2),
        nextCursor: "page_2"
      };
    } else if (cursor === "page_2") {
      // 第二页返回后两个工具，没有 nextCursor
      response.result = {
        tools: ALL_TOOLS.slice(2)
      };
    } else {
      response.error = { code: -32602, message: `Invalid cursor: ${cursor}` };
    }
  } else if (method === 'tools/call') {
    const toolName = params ? params.name : undefined;
    const args = params ? params.arguments : undefined;
    
    if (!toolName) {
      response.error = { code: -32602, message: "Missing tool name" };
    } else if (toolName === 'calculate_add') {
      if (!args || args.a === undefined || args.b === undefined) {
        response.error = { code: -32602, message: "Missing required arguments: a or b" };
      } else {
        const a = Number(args.a);
        const b = Number(args.b);
        response.result = {
          content: [{ type: "text", text: `计算成功，结果为: ${a + b}` }]
        };
      }
    } else if (toolName === 'get_system_time') {
      response.result = {
        content: [{ type: "text", text: `系统当前时间为: ${new Date().toISOString()}` }]
      };
    } else if (toolName === 'get_system_info') {
      response.result = {
        content: [{ type: "text", text: `Mock 操作系统版本: Windows 11 PRO (Mocked by JS), 运行正常` }]
      };
    } else if (toolName === 'trigger_exception') {
      response.result = {
        content: [{ type: "text", text: "触发测试异常：数据库连接失败。" }],
        isError: true
      };
    } else {
      response.error = { code: -32601, message: `Tool not found: ${toolName}` };
    }
  } else if (method === 'resources/list') {
    response.result = { resources: RESOURCES };
  } else if (method === 'resources/read') {
    const uri = params.uri;
    if (uri === "file:///logs/system.log") {
      response.result = {
        contents: [{
          uri: uri,
          mimeType: "text/plain",
          text: "[2026-06-25 12:00:00] SYSTEM INFO: MCP Core components started. Normal status."
        }]
      };
    } else if (uri === "file:///configs/app.json") {
      response.result = {
        contents: [{
          uri: uri,
          mimeType: "application/json",
          text: "{\n  \"env\": \"test\",\n  \"debug\": true\n}"
        }]
      };
    } else {
      response.error = { code: -32602, message: `Resource not found: ${uri}` };
    }
  } else if (method === 'prompts/list') {
    response.result = { prompts: PROMPTS };
  } else if (method === 'prompts/get') {
    const name = params.name;
    const args = params.arguments || {};
    if (name === 'code_review') {
      response.result = {
        description: "代码审查指令模板",
        messages: [{
          role: "user",
          content: {
            type: "text",
            text: `请帮我审查以下 C++ 代码，指出其潜在的内存泄漏或性能风险：\n\n${args.code || "(未提供代码)"}`
          }
        }]
      };
    } else {
      response.error = { code: -32602, message: `Prompt not found: ${name}` };
    }
  } else {
    response.error = { code: -32601, message: `Method not found: ${method}` };
  }

  return response;
}

if (isHttp) {
  const PORT = 3000;
  const HOST = '127.0.0.1'; // 仅监听 127.0.0.1，绝不监听 0.0.0.0 (防范 DNS Rebinding)
  const activeSessions = new Map();

  const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    const pathname = parsedUrl.pathname;

    // 1. Origin 校验以防 DNS Rebinding 攻击
    const origin = req.headers['origin'];
    if (origin) {
      try {
        const originUrl = new URL(origin);
        if (originUrl.hostname !== 'localhost' && originUrl.hostname !== '127.0.0.1') {
          res.writeHead(403, { 'Content-Type': 'text/plain; charset=utf-8' });
          res.end('Forbidden: Invalid Origin (DNS Rebinding protection)\n');
          return;
        }
      } catch (err) {
        res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('Bad Request: Malformed Origin header\n');
        return;
      }
    }

    // 2. 校验协议版本首部 MCP-Protocol-Version
    const protocolVersion = req.headers['mcp-protocol-version'];
    if (!protocolVersion || protocolVersion !== '2025-11-25') {
      res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Bad Request: Missing or unsupported MCP-Protocol-Version\n');
      return;
    }

    // 3. 路由分发
    if (pathname === '/mcp') {
      if (req.method === 'GET') {
        const accept = req.headers['accept'] || '';
        if (!accept.includes('text/event-stream') && !accept.includes('application/json')) {
          res.writeHead(406, { 'Content-Type': 'text/plain' });
          res.end('Not Acceptable: Requires text/event-stream\n');
          return;
        }

        // 输出重连时的 Last-Event-ID
        const lastEventId = req.headers['last-event-id'];
        if (lastEventId) {
          console.error(`[Server] Client reconnected with Last-Event-ID: ${lastEventId}`);
        }

        res.writeHead(200, {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache',
          'Connection': 'keep-alive',
          'Access-Control-Allow-Origin': origin || '*',
          'MCP-Protocol-Version': '2025-11-25'
        });

        // 持续发送心跳，防止连接超时断开
        const keepAliveTimer = setInterval(() => {
          res.write(': keepalive\n\n');
        }, 15000);

        const sessionId = 'session_' + Math.random().toString(36).substring(2, 10);
        const sessionInfo = {
          res: res,
          eventId: 1,
          keepAliveTimer: keepAliveTimer
        };
        activeSessions.set(sessionId, sessionInfo);

        req.on('close', () => {
          clearInterval(keepAliveTimer);
          activeSessions.delete(sessionId);
          console.error(`[Server] SSE Session ${sessionId} closed.`);
        });

        // 推送初始的 endpoint 事件，指示 POST 的目的 URL 包含 sessionId
        res.write(`id: ${sessionInfo.eventId}\n`);
        res.write(`event: endpoint\n`);
        res.write(`data: /mcp?sessionId=${sessionId}\n\n`);

        console.error(`[Server] SSE Session established: ${sessionId}`);

      } else if (req.method === 'POST') {
        const sessionId = parsedUrl.query.sessionId;
        if (!sessionId || !activeSessions.has(sessionId)) {
          res.writeHead(400, { 'Content-Type': 'text/plain' });
          res.end('Bad Request: Invalid or missing sessionId\n');
          return;
        }

        const session = activeSessions.get(sessionId);

        let body = '';
        req.on('data', chunk => {
          body += chunk.toString();
        });

        req.on('end', () => {
          try {
            const request = JSON.parse(body);
            // HTTP POST 仅用于投递消息，立即返回 202 Accepted 状态，实际响应异步由 SSE 下发
            res.writeHead(202, {
              'Content-Type': 'application/json',
              'Access-Control-Allow-Origin': origin || '*'
            });
            res.end();

            // 特殊测试钩子：触发意外断线以验证客户端自动重连
            if (request && request.method === 'test/trigger_disconnect') {
              console.error(`[Server] Triggering manual SSE disconnect for session ${sessionId} as requested.`);
              session.res.end();
              return;
            }

            const response = processMcpRequest(request);
            if (response) {
              session.eventId++;
              session.res.write(`id: ${session.eventId}\n`);
              session.res.write(`event: message\n`);
              session.res.write(`data: ${JSON.stringify(response)}\n\n`);
            }
          } catch (err) {
            res.writeHead(400, { 'Content-Type': 'text/plain' });
            res.end('Bad Request: Invalid JSON payload\n');
          }
        });
      } else {
        res.writeHead(405, { 'Content-Type': 'text/plain' });
        res.end('Method Not Allowed\n');
      }
    } else {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('Not Found\n');
    }
  });

  server.listen(PORT, HOST, () => {
    console.error(`[Server] MCP Streamable HTTP server listening on http://${HOST}:${PORT}/mcp`);
  });
} else {
  // Stdio 模式
  // 故意在 stdout 输出非 JSON 的脏文本，用于集成验证客户端的拦截过滤与警告机制
  process.stdout.write("Mock Server Started Successfully (Dirty Log for Testing)\n");

  const readline = require('readline');
  const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    terminal: false
  });

  rl.on('line', (line) => {
    if (!line.trim()) return;
    try {
      const request = JSON.parse(line);
      const response = processMcpRequest(request);
      if (response) {
        process.stdout.write(JSON.stringify(response) + "\n");
      }
    } catch (err) {
      process.stderr.write(`Error: ${err.message}\n`);
    }
  });
}
