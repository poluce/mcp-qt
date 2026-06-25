// test_mcp_server.js
// 这是一个支持 Stdio 和 HTTP/SSE (Streamable HTTP) 双重协议的 Mock MCP 服务端，用于集成测试。
const http = require('http');
const url = require('url');

const isHttp = process.argv.includes('--http');

// 支持的全部工具列表
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
  },
  {
    name: "delay_timeout",
    description: "延迟响应的测试工具",
    inputSchema: {
      type: "object",
      properties: {
        delayMs: { type: "number", description: "延迟的毫秒数" }
      }
    }
  }
];

// 支持的全部资源列表
const ALL_RESOURCES = [
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
  },
  {
    uri: "file:///configs/admin.json",
    name: "系统管理员配置文件",
    mimeType: "application/json",
    description: "受保护的管理员机密配置（权限测试用）"
  },
  {
    uri: "file:///logs/huge.log",
    name: "巨大日志文件",
    mimeType: "text/plain",
    description: "用于压力测试的大型文本资源（大文件测试用）"
  },
  {
    uri: "file:///assets/logo.png",
    name: "二进制标志图像",
    mimeType: "image/png",
    description: "MCP 的官方标志图片（二进制/MIME测试用）"
  }
];

// 支持的全部提示词模版
const ALL_PROMPTS = [
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
  },
  {
    name: "rich_prompt",
    description: "测试包含文本、图像和内嵌资源的富媒体提示词模版",
    arguments: []
  }
];

// 核心 JSON-RPC 处理逻辑，由 Stdio 与 HTTP/SSE 共用
function processMcpRequest(request) {
  if (typeof request !== 'object' || request === null) {
    return {
      jsonrpc: "2.0",
      id: null,
      error: { code: -32600, message: "Invalid Request: Payload must be a JSON object" }
    };
  }

  const { id, method, params, jsonrpc } = request;

  // 1. 校验 jsonrpc 协议版本
  if (jsonrpc !== "2.0") {
    return {
      jsonrpc: "2.0",
      id: id !== undefined ? id : null,
      error: { code: -32600, message: "Invalid Request: Missing or invalid jsonrpc version" }
    };
  }

  // 2. 校验 id 类型是否合法
  if (id !== undefined && id !== null && typeof id !== 'number' && typeof id !== 'string') {
    return {
      jsonrpc: "2.0",
      id: null,
      error: { code: -32600, message: "Invalid Request: id must be a string, number, or null" }
    };
  }

  // 3. 校验 method 是否合法
  if (method === undefined || typeof method !== 'string') {
    return {
      jsonrpc: "2.0",
      id: id !== undefined ? id : null,
      error: { code: -32600, message: "Invalid Request: method is required and must be a string" }
    };
  }

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
        resources: { subscribe: true, listChanged: true },
        prompts: { listChanged: true }
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
        if (isNaN(a) || isNaN(b)) {
          response.error = { code: -32602, message: "Invalid params: a and b must be numbers" };
        } else {
          response.result = {
            content: [{ type: "text", text: `计算成功，结果为: ${a + b}` }]
          };
        }
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
    } else if (toolName === 'delay_timeout') {
      const delayMs = args && args.delayMs !== undefined ? Number(args.delayMs) : 10000;
      setTimeout(() => {
        const delayedResponse = {
          jsonrpc: "2.0",
          id: id,
          result: {
            content: [{ type: "text", text: `延迟了 ${delayMs} 毫秒的响应` }]
          }
        };
        if (isHttp) {
          const session = activeSessions.get(sessionId);
          if (session) {
            session.eventId++;
            session.res.write(`id: ${session.eventId}\n`);
            session.res.write(`event: message\n`);
            session.res.write(`data: ${JSON.stringify(delayedResponse)}\n\n`);
          }
        } else {
          process.stdout.write(JSON.stringify(delayedResponse) + "\n");
        }
      }, delayMs);
      return null; // 异步响应，现在不返回
    } else {
      response.error = { code: -32601, message: `Tool not found: ${toolName}` };
    }
  } else if (method === 'resources/list') {
    const cursor = params ? params.cursor : undefined;
    if (!cursor) {
      response.result = {
        resources: ALL_RESOURCES.slice(0, 3),
        nextCursor: "res_page_2"
      };
    } else if (cursor === "res_page_2") {
      response.result = {
        resources: ALL_RESOURCES.slice(3)
      };
    } else {
      response.error = { code: -32602, message: `Invalid cursor: ${cursor}` };
    }
  } else if (method === 'resources/read') {
    const uri = params ? params.uri : undefined;
    if (!uri) {
      response.error = { code: -32602, message: "Missing required parameter: uri" };
    } else if (uri === "file:///logs/system.log") {
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
    } else if (uri === "file:///configs/admin.json") {
      // 权限不足校验
      response.error = {
        code: -32000,
        message: "Permission Denied: admin config requires root privileges"
      };
    } else if (uri === "file:///logs/huge.log") {
      // 大文件校验 (2MB)
      response.result = {
        contents: [{
          uri: uri,
          mimeType: "text/plain",
          text: "H".repeat(2 * 1024 * 1024)
        }]
      };
    } else if (uri === "file:///assets/logo.png") {
      // 二进制资源与 MIME type 校验
      response.result = {
        contents: [{
          uri: uri,
          mimeType: "image/png",
          blob: "iVBORw0KGgoAAAANSUhEUgAAAAUAAAAFCAYAAACNbyblAAAAHElEQVQI12P4//8/w38GIAXDIBKE0DHxgljNBAAO9TXL0Y4OHwAAAABJRU5ErkJggg=="
        }]
      };
    } else {
      response.error = { code: -32602, message: `Resource not found: ${uri}` };
    }
  } else if (method === 'resources/subscribe') {
    response.result = {}; // 订阅成功返回空结果
  } else if (method === 'resources/unsubscribe') {
    response.result = {}; // 取消订阅成功返回空结果
  } else if (method === 'prompts/list') {
    const cursor = params ? params.cursor : undefined;
    if (!cursor) {
      response.result = {
        prompts: ALL_PROMPTS.slice(0, 1),
        nextCursor: "prompt_page_2"
      };
    } else if (cursor === "prompt_page_2") {
      response.result = {
        prompts: ALL_PROMPTS.slice(1)
      };
    } else {
      response.error = { code: -32602, message: `Invalid cursor: ${cursor}` };
    }
  } else if (method === 'prompts/get') {
    const name = params ? params.name : undefined;
    const args = params ? params.arguments || {} : {};
    
    if (!name) {
      response.error = { code: -32602, message: "Missing required parameter: name" };
    } else if (name === 'code_review') {
      const codeArg = args.code;
      if (codeArg === undefined) {
        response.error = { code: -32602, message: "Missing required argument: code" };
      } else if (typeof codeArg !== 'string') {
        response.error = { code: -32602, message: "Argument 'code' must be a string" };
      } else {
        response.result = {
          description: "代码审查指令模板",
          messages: [{
            role: "user",
            content: {
              type: "text",
              text: `请帮我审查以下 C++ 代码，指出其潜在的内存泄漏或性能风险：\n\n${codeArg}`
            }
          }]
        };
      }
    } else if (name === 'rich_prompt') {
      // 复合多介质返回：text content, image content, embedded resource content
      response.result = {
        description: "复合提示词结果",
        messages: [{
          role: "assistant",
          content: [
            {
              type: "text",
              text: "这里是一个包含多种内容的提示词回复："
            },
            {
              type: "image",
              data: "iVBORw0KGgoAAAANSUhEUgAAAAUAAAAFCAYAAACNbyblAAAAHElEQVQI12P4//8/w38GIAXDIBKE0DHxgljNBAAO9TXL0Y4OHwAAAABJRU5ErkJggg==",
              mimeType: "image/png"
            },
            {
              type: "resource",
              resource: {
                uri: "file:///logs/system.log",
                text: "[Embedded Log] System is running in perfect condition."
              }
            }
          ]
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

            // 特殊测试钩子 A：触发意外断线以验证客户端自动重连
            if (request && request.method === 'test/trigger_disconnect') {
              console.error(`[Server] Triggering manual SSE disconnect for session ${sessionId} as requested.`);
              session.res.end();
              return;
            }

            // 特殊测试钩子 B：向客户端发送资源更新变更通知
            if (request && request.method === 'test/trigger_resource_update') {
              const targetUri = request.params ? request.params.uri : "file:///logs/system.log";
              console.error(`[Server] Triggering resources/updated notification for ${targetUri}`);
              session.eventId++;
              session.res.write(`id: ${session.eventId}\n`);
              session.res.write(`event: message\n`);
              session.res.write(`data: ${JSON.stringify({
                jsonrpc: "2.0",
                method: "notifications/resources/updated",
                params: { uri: targetUri }
              })}\n\n`);
              return;
            }

            // 特殊测试钩子 C：向客户端发送提示词变更通知
            if (request && request.method === 'test/trigger_prompts_changed') {
              console.error(`[Server] Triggering prompts/list-changed notification`);
              session.eventId++;
              session.res.write(`id: ${session.eventId}\n`);
              session.res.write(`event: message\n`);
              session.res.write(`data: ${JSON.stringify({
                jsonrpc: "2.0",
                method: "notifications/prompts/list-changed",
                params: {}
              })}\n\n`);
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
      const parseErrorResponse = {
        jsonrpc: "2.0",
        id: null,
        error: { code: -32700, message: "Parse error: " + err.message }
      };
      process.stdout.write(JSON.stringify(parseErrorResponse) + "\n");
    }
  });
}
