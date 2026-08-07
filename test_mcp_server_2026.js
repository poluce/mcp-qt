// test_mcp_server_2026.js
// 符合 MCP 2026-07-28 规范的无状态 Streamable HTTP 测试服务器。
//
// 与 test_mcp_server.js（2025-11-25 时代、会话型 GET+POST）不同，本服务器实现
// 2026-07-28 的核心 wire 语义：
//   - 单一 POST endpoint，无会话、无 Mcp-Session-Id、无 GET SSE 流
//   - server/discover RPC（服务器 MUST 实现，SEP-2575）
//   - 每请求 MCP-Protocol-Version header（MUST）且与 body _meta.io.modelcontextprotocol/protocolVersion 一致
//   - Mcp-Method / Mcp-Name header 路由校验（SEP-2243），不匹配返回 HeaderMismatch(-32020)
//   - 版本不支持返回 UnsupportedProtocolVersionError(-32022, 带 supported 列表)
//   - 未知方法返回 404 + -32601
//   - 所有结果携带 resultType（"complete"/"input_required"，SEP-2322）
//   - list 类结果携带 ttlMs/cacheScope（CacheableResult，SEP-2549）
//   - MRTR 演示：approve_delete 工具首次调用返回 input_required + requestState
//   - 资源 not found 使用 -32602（不再是 -32002，SEP-2164）
//   - 通知 POST -> 202 Accepted 无 body；GET/DELETE -> 405
//   - Origin 校验（防 DNS Rebinding）
//   - legacy initialize 请求 -> -32022（modern-only 服务器应告知支持的版本）

'use strict';

const http = require('http');

const PORT = 3001;
const HOST = '127.0.0.1'; // 只监听 127.0.0.1，绝不监听 0.0.0.0
const MCP_ENDPOINT = '/mcp';

// 服务器支持的协议版本（本实现为 modern 时代）
const SUPPORTED_VERSIONS = ['2026-07-28'];
const SERVER_INFO = { name: 'mock-test-server-2026', version: '2.0.0' };
const SERVER_INSTRUCTIONS =
  '2026-07-28 stateless mock server. Use server/discover to learn capabilities; ' +
  'tools/approve_delete demonstrates Multi Round-Trip Requests.';

// 支持的全部工具
const ALL_TOOLS = [
  {
    name: 'calculate_add',
    description: '计算两个数字的和',
    inputSchema: {
      type: 'object',
      properties: {
        a: { type: 'number', description: '第一个数字' },
        b: { type: 'number', description: '第二个数字' }
      },
      required: ['a', 'b']
    }
  },
  {
    name: 'get_system_time',
    description: '获取系统当前时间（无参数测试工具）',
    inputSchema: { type: 'object', properties: {} }
  },
  {
    name: 'trigger_exception',
    description: '触发工具内部异常的测试工具',
    inputSchema: { type: 'object', properties: {} }
  },
  {
    name: 'approve_delete',
    description: 'MRTR 演示：首次调用要求客户端确认删除，重发带 inputResponses 后完成',
    inputSchema: {
      type: 'object',
      properties: {
        target: { type: 'string', description: '要删除的目标名称' },
        // x-mcp-header 注解示例（SEP-2243）：参数 region 镜像为 Mcp-Param-Region 头
        region: { type: 'string', description: '部署区域', 'x-mcp-header': 'Region' }
      },
      required: ['target']
    }
  }
];

const ALL_RESOURCES = [
  {
    uri: 'file:///logs/system.log',
    name: '系统日志文件',
    mimeType: 'text/plain',
    description: '保存系统运行时的核心状态日志'
  },
  {
    uri: 'file:///configs/app.json',
    name: '应用配置文件',
    mimeType: 'application/json',
    description: '本地调试应用的配置文件'
  }
];

const ALL_PROMPTS = [
  {
    name: 'code_review',
    description: '对指定的 C++ 代码进行静态分析和代码审查',
    arguments: [
      { name: 'code', description: '待审查的代码文本内容', required: true }
    ]
  }
];

// ---------- JSON-RPC helpers ----------

function resultResponse(id, result) {
  return { jsonrpc: '2.0', id, result };
}

function errorResponse(id, code, message, data) {
  const error = { code, message };
  if (data !== undefined) error.data = data;
  return { jsonrpc: '2.0', id, error };
}

// 为普通结果补充规范必填的 resultType（SEP-2322）
function complete(result) {
  result.resultType = 'complete';
  return result;
}

// 为 list 类结果补充 CacheableResult 字段（SEP-2549）
function cacheable(result) {
  result.ttlMs = 60000;
  result.cacheScope = 'private';
  return result;
}

// ---------- Header 校验（SEP-2243 / Server Validation） ----------

// 解码 Base64 sentinel 编码的 header 值（=?base64?...?=）
function decodeHeaderValue(value) {
  const m = /^=\?base64\?(.+)\?=$/.exec(value);
  if (m) {
    try {
      return Buffer.from(m[1], 'base64').toString('utf8');
    } catch (e) {
      return value;
    }
  }
  return value;
}

// 校验通过返回 null，否则返回 { status, response }
function validateHeaders(headers, body) {
  const id = body && body.id !== undefined ? body.id : null;

  const protoHeader = headers['mcp-protocol-version'];
  if (!protoHeader) {
    return {
      status: 400,
      response: errorResponse(id, -32020,
        'HeaderMismatch: Missing required MCP-Protocol-Version header')
    };
  }

  // header 与 body _meta 中版本必须一致（Server Validation）
  const metaVersion = body && body.params && body.params._meta &&
    body.params._meta['io.modelcontextprotocol/protocolVersion'];
  if (metaVersion !== undefined && metaVersion !== protoHeader) {
    return {
      status: 400,
      response: errorResponse(id, -32020,
        `HeaderMismatch: MCP-Protocol-Version header '${protoHeader}' does not match body _meta '${metaVersion}'`)
    };
  }

  if (!SUPPORTED_VERSIONS.includes(protoHeader)) {
    return {
      status: 400,
      response: errorResponse(id, -32022, 'Unsupported protocol version',
        { supported: SUPPORTED_VERSIONS, requested: protoHeader })
    };
  }

  const methodHeader = headers['mcp-method'];
  if (!methodHeader) {
    return {
      status: 400,
      response: errorResponse(id, -32020, 'HeaderMismatch: Missing required Mcp-Method header')
    };
  }
  if (methodHeader !== body.method) {
    return {
      status: 400,
      response: errorResponse(id, -32020,
        `HeaderMismatch: Mcp-Method header '${methodHeader}' does not match body method '${body.method}'`)
    };
  }

  // Mcp-Name 对 tools/call、resources/read、prompts/get 是 REQUIRED
  if (['tools/call', 'resources/read', 'prompts/get'].includes(body.method)) {
    const nameHeader = headers['mcp-name'];
    const bodyName = body.method === 'resources/read'
      ? (body.params && body.params.uri)
      : (body.params && body.params.name);
    if (!nameHeader) {
      return {
        status: 400,
        response: errorResponse(id, -32020,
          `HeaderMismatch: Missing required Mcp-Name header for ${body.method}`)
      };
    }
    const decodedName = decodeHeaderValue(nameHeader);
    if (bodyName !== undefined && decodedName !== bodyName) {
      return {
        status: 400,
        response: errorResponse(id, -32020,
          `HeaderMismatch: Mcp-Name header '${nameHeader}' does not match body value '${bodyName}'`)
      };
    }
  }

  // x-mcp-header 注解参数镜像为 Mcp-Param-{Name} 头，必须与 body 一致（示例工具 approve_delete.region）
  const method = body.method;
  if (method === 'tools/call' && body.params && body.params.name === 'approve_delete') {
    const region = body.params.arguments && body.params.arguments.region;
    if (region !== undefined && region !== null) {
      const regionHeader = headers['mcp-param-region'];
      if (!regionHeader) {
        return {
          status: 400,
          response: errorResponse(id, -32020,
            'HeaderMismatch: Missing required Mcp-Param-Region header for approve_delete')
        };
      }
      const decodedRegion = decodeHeaderValue(regionHeader);
      if (String(decodedRegion) !== String(region)) {
        return {
          status: 400,
          response: errorResponse(id, -32020,
            `HeaderMismatch: Mcp-Param-Region header '${regionHeader}' does not match body value '${region}'`)
        };
      }
    }
  }

  return null;
}

// ---------- RPC 处理（无状态，逐请求独立） ----------

// 返回 { handled: bool, status, response | null }
// handled=true 表示由本函数决定 HTTP 响应；handled=false 表示内部错误（如未知方法走 404）
function processMcpRequest(body, headers) {
  if (typeof body !== 'object' || body === null) {
    return { handled: true, status: 400, response: errorResponse(null, -32600, 'Invalid Request: Payload must be a JSON object') };
  }

  const { id, method, params, jsonrpc } = body;

  if (jsonrpc !== '2.0') {
    return { handled: true, status: 400, response: errorResponse(id !== undefined ? id : null, -32600, 'Invalid Request: Missing or invalid jsonrpc version') };
  }
  if (id !== undefined && id !== null && typeof id !== 'number' && typeof id !== 'string') {
    return { handled: true, status: 400, response: errorResponse(null, -32600, 'Invalid Request: id must be a string, number, or null') };
  }
  if (method === undefined || typeof method !== 'string') {
    return { handled: true, status: 400, response: errorResponse(id !== undefined ? id : null, -32600, 'Invalid Request: method is required and must be a string') };
  }

  // 通知（无 id）：HTTP 上 202 Accepted 无 body；除取消外本规范无客户端通知语义
  if (id === undefined) {
    return { handled: true, status: 202, response: null };
  }

  // Header 校验（在方法分发之前）
  const validation = validateHeaders(headers, body);
  if (validation) {
    return { handled: true, status: validation.status, response: validation.response };
  }

  switch (method) {
    case 'server/discover': {
      const result = complete({
        supportedVersions: SUPPORTED_VERSIONS,
        capabilities: {
          tools: { listChanged: false },
          resources: { subscribe: false, listChanged: false },
          prompts: { listChanged: false }
        },
        _meta: { 'io.modelcontextprotocol/serverInfo': SERVER_INFO },
        instructions: SERVER_INSTRUCTIONS
      });
      return {
        handled: true,
        status: 200,
        response: resultResponse(id, cacheable(result))
      };
    }

    case 'tools/list': {
      const cursor = params ? params.cursor : undefined;
      if (!cursor) {
        return {
          handled: true, status: 200,
          response: resultResponse(id, cacheable(complete({ tools: ALL_TOOLS.slice(0, 2), nextCursor: 'page_2' })))
        };
      }
      if (cursor === 'page_2') {
        return {
          handled: true, status: 200,
          response: resultResponse(id, cacheable(complete({ tools: ALL_TOOLS.slice(2) })))
        };
      }
      return { handled: true, status: 400, response: errorResponse(id, -32602, `Invalid cursor: ${cursor}`) };
    }

    case 'tools/call': {
      const toolName = params ? params.name : undefined;
      const args = params ? params.arguments : undefined;

      if (!toolName) {
        return { handled: true, status: 400, response: errorResponse(id, -32602, 'Missing tool name') };
      }

      if (toolName === 'calculate_add') {
        if (!args || args.a === undefined || args.b === undefined) {
          return { handled: true, status: 400, response: errorResponse(id, -32602, 'Missing required arguments: a or b') };
        }
        const a = Number(args.a);
        const b = Number(args.b);
        if (isNaN(a) || isNaN(b)) {
          return { handled: true, status: 400, response: errorResponse(id, -32602, 'Invalid params: a and b must be numbers') };
        }
        return {
          handled: true, status: 200,
          response: resultResponse(id, complete({ content: [{ type: 'text', text: `计算成功，结果为: ${a + b}` }] }))
        };
      }

      if (toolName === 'get_system_time') {
        return {
          handled: true, status: 200,
          response: resultResponse(id, complete({ content: [{ type: 'text', text: `系统当前时间为: ${new Date().toISOString()}` }] }))
        };
      }

      if (toolName === 'trigger_exception') {
        return {
          handled: true, status: 200,
          response: resultResponse(id, complete({ content: [{ type: 'text', text: '触发测试异常：数据库连接失败。' }], isError: true }))
        };
      }

      if (toolName === 'approve_delete') {
        const target = args && args.target;
        if (!target) {
          return { handled: true, status: 400, response: errorResponse(id, -32602, 'Missing required argument: target') };
        }
        // MRTR（SEP-2322）：无 inputResponses 时返回 input_required
        if (!params.inputResponses) {
          return {
            handled: true,
            status: 200,
            response: resultResponse(id, {
              resultType: 'input_required',
              inputRequests: {
                confirm: {
                  method: 'elicitation/create',
                  params: {
                    mode: 'form',
                    message: `确认删除 ${target} 吗？`,
                    requestedSchema: {
                      type: 'object',
                      properties: { confirm: { type: 'boolean' } },
                      required: ['confirm']
                    }
                  }
                }
              },
              requestState: '2026-mock-state-aead-1'
            })
          };
        }
        const confirm = params.inputResponses.confirm &&
          params.inputResponses.confirm.content &&
          params.inputResponses.confirm.content.confirm;
        if (confirm === true || confirm === 'true' || confirm === 1) {
          return {
            handled: true, status: 200,
            response: resultResponse(id, complete({ content: [{ type: 'text', text: `已删除: ${target}` }] }))
          };
        }
        return {
          handled: true, status: 200,
          response: resultResponse(id, complete({ content: [{ type: 'text', text: `已取消删除: ${target}` }] }))
        };
      }

      return { handled: true, status: 400, response: errorResponse(id, -32601, `Tool not found: ${toolName}`) };
    }

    case 'resources/list': {
      const cursor = params ? params.cursor : undefined;
      if (!cursor) {
        return {
          handled: true, status: 200,
          response: resultResponse(id, cacheable(complete({ resources: ALL_RESOURCES.slice(0, 1), nextCursor: 'res_page_2' })))
        };
      }
      if (cursor === 'res_page_2') {
        return {
          handled: true, status: 200,
          response: resultResponse(id, cacheable(complete({ resources: ALL_RESOURCES.slice(1) })))
        };
      }
      return { handled: true, status: 400, response: errorResponse(id, -32602, `Invalid cursor: ${cursor}`) };
    }

    case 'resources/read': {
      const uri = params ? params.uri : undefined;
      if (!uri) {
        return { handled: true, status: 400, response: errorResponse(id, -32602, 'Missing required parameter: uri') };
      }
      if (uri === 'file:///logs/system.log') {
        return {
          handled: true, status: 200,
          response: resultResponse(id, cacheable(complete({
            contents: [{
              uri,
              mimeType: 'text/plain',
              text: '[2026-06-25 12:00:00] SYSTEM INFO: 2026-07-28 stateless server is running. Normal status.'
            }]
          })))
        };
      }
      if (uri === 'file:///configs/app.json') {
        return {
          handled: true, status: 200,
          response: resultResponse(id, cacheable(complete({
            contents: [{
              uri,
              mimeType: 'application/json',
              text: '{\n  "env": "test",\n  "debug": true\n}'
            }]
          })))
        };
      }
      // SEP-2164：资源不存在错误码从 -32002 改为 -32602
      return { handled: true, status: 200, response: errorResponse(id, -32602, `Resource not found: ${uri}`) };
    }

    case 'prompts/list': {
      const cursor = params ? params.cursor : undefined;
      if (!cursor) {
        return {
          handled: true, status: 200,
          response: resultResponse(id, cacheable(complete({ prompts: ALL_PROMPTS, nextCursor: undefined })))
        };
      }
      return { handled: true, status: 400, response: errorResponse(id, -32602, `Invalid cursor: ${cursor}`) };
    }

    case 'prompts/get': {
      const name = params ? params.name : undefined;
      const args = params ? params.arguments || {} : {};
      if (!name) {
        return { handled: true, status: 400, response: errorResponse(id, -32602, 'Missing required parameter: name') };
      }
      if (name === 'code_review') {
        const codeArg = args.code;
        if (codeArg === undefined) {
          return { handled: true, status: 400, response: errorResponse(id, -32602, 'Missing required argument: code') };
        }
        return {
          handled: true, status: 200,
          response: resultResponse(id, complete({
            description: '代码审查指令模板',
            messages: [{
              role: 'user',
              content: { type: 'text', text: `请帮我审查以下 C++ 代码，指出其潜在的内存泄漏或性能风险：\n\n${codeArg}` }
            }]
          }))
        };
      }
      return { handled: true, status: 400, response: errorResponse(id, -32602, `Prompt not found: ${name}`) };
    }

    case 'initialize':
      // legacy 客户端握手：modern-only 服务器返回 UnsupportedProtocolVersionError 并列出支持的版本
      return {
        handled: true,
        status: 400,
        response: errorResponse(id, -32022, 'Unsupported protocol version',
          { supported: SUPPORTED_VERSIONS, requested: (params && params.protocolVersion) || 'legacy initialize' })
      };

    case 'ping':
    case 'logging/setLevel':
    case 'resources/subscribe':
    case 'resources/unsubscribe':
    case 'notifications/roots/list_changed':
      // 2026-07-28 已移除的方法：按未知方法处理
      return { handled: true, status: 404, response: errorResponse(id, -32601, `Method not found: ${method}`) };

    default:
      return { handled: true, status: 404, response: errorResponse(id, -32601, `Method not found: ${method}`) };
  }
}

// ---------- HTTP 服务 ----------

const server = http.createServer((req, res) => {
  const pathname = new URL(req.url, `http://${req.headers.host}`).pathname;

  // 1. Origin 校验（防 DNS Rebinding）
  const origin = req.headers.origin;
  if (origin) {
    try {
      const originUrl = new URL(origin);
      if (originUrl.hostname !== 'localhost' && originUrl.hostname !== '127.0.0.1') {
        res.writeHead(403, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(errorResponse(null, -32000, 'Forbidden: Invalid Origin (DNS Rebinding protection)')));
        return;
      }
    } catch (err) {
      res.writeHead(400, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(errorResponse(null, -32000, 'Bad Request: Malformed Origin header')));
      return;
    }
  }

  // 2. 仅接受单一 POST MCP endpoint
  if (pathname !== MCP_ENDPOINT) {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('Not Found\n');
    return;
  }

  // 2026-07-28 移除 GET SSE 流 / DELETE 会话：modern-only 服务器返回 405
  if (req.method !== 'POST') {
    res.writeHead(405, { 'Content-Type': 'text/plain', Allow: 'POST' });
    res.end('Method Not Allowed: only POST is supported\n');
    return;
  }

  let body = '';
  req.on('data', chunk => { body += chunk.toString(); });
  req.on('end', () => {
    let request;
    try {
      request = JSON.parse(body);
    } catch (err) {
      res.writeHead(400, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(errorResponse(null, -32700, 'Parse error: ' + err.message)));
      return;
    }

    const outcome = processMcpRequest(request, req.headers);

    if (outcome.response === null) {
      // 通知 -> 202 Accepted 无 body
      res.writeHead(outcome.status, { 'Content-Type': 'application/json' });
      res.end();
      return;
    }

    const payload = JSON.stringify(outcome.response);
    res.writeHead(outcome.status, {
      'Content-Type': 'application/json',
      'X-Accel-Buffering': 'no'
    });
    res.end(payload);
  });
});

server.listen(PORT, HOST, () => {
  console.error(`[Server] MCP 2026-07-28 stateless server listening on http://${HOST}:${PORT}${MCP_ENDPOINT}`);
  console.error(`[Server] Supported protocol versions: ${SUPPORTED_VERSIONS.join(', ')}`);
});
