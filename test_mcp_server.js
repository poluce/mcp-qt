// test_mcp_server.js
// 这是一个极简的、零依赖的 Mock MCP Stdio 服务端，用于验证 C++ MCP SDK 功能
const readline = require('readline');

// 故意在 stdout 输出非 JSON 的脏文本，用于集成验证客户端的拦截过滤与警告机制
process.stdout.write("Mock Server Started Successfully (Dirty Log for Testing)\n");

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  terminal: false
});

// 支持的工具列表
const TOOLS = [
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
    name: "get_system_info",
    description: "获取系统版本及当前状态",
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

rl.on('line', (line) => {
  if (!line.trim()) return;
  try {
    const request = JSON.parse(line);
    const { id, method, params } = request;

    if (id === undefined) return; // 忽略通知

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
      process.stdout.write(JSON.stringify(response) + "\n");
      // 延迟 100ms 退出进程
      setTimeout(() => {
        process.exit(0);
      }, 100);
      return;
    } else if (method === 'tools/list') {
      response.result = { tools: TOOLS };
    } else if (method === 'tools/call') {
      const toolName = params.name;
      const args = params.arguments || {};
      
      if (toolName === 'calculate_add') {
        const a = Number(args.a || 0);
        const b = Number(args.b || 0);
        response.result = {
          content: [{ type: "text", text: `计算成功，结果为: ${a + b}` }]
        };
      } else if (toolName === 'get_system_info') {
        response.result = {
          content: [{ type: "text", text: `Mock 操作系统版本: Windows 11 PRO (Mocked by JS), 运行正常` }]
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

    process.stdout.write(JSON.stringify(response) + "\n");
  } catch (err) {
    process.stderr.write(`Error: ${err.message}\n`);
  }
});
