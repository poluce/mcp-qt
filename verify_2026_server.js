// verify_2026_server.js
// 对 test_mcp_server_2026.js 做 wire 级符合性验证（不依赖任何 MCP 客户端库）。
// 逐条对照 MCP 2026-07-28 规范断言；任何失败以非零退出码结束。
// 用法: node verify_2026_server.js [endpointUrl]
'use strict';

const BASE = process.argv[2] || 'http://127.0.0.1:3001/mcp';
const PROTOCOL_VERSION = '2026-07-28';
const MC_HEADERS = {
  'Content-Type': 'application/json',
  'MCP-Protocol-Version': PROTOCOL_VERSION
};

let passed = 0;
let failed = 0;

function check(name, cond, detail) {
  if (cond) {
    passed++;
    console.log(`  PASS  ${name}`);
  } else {
    failed++;
    console.log(`  FAIL  ${name}${detail ? ' -> ' + detail : ''}`);
  }
}

async function post(body, extraHeaders = {}) {
  const headers = Object.assign({}, MC_HEADERS, extraHeaders);
  if (body !== null) headers['Content-Type'] = 'application/json';
  // Mcp-Method header 对所有请求 REQUIRED（SEP-2243）
  if (body && body.method && !headers['Mcp-Method']) {
    headers['Mcp-Method'] = body.method;
  }
  const res = await fetch(BASE, {
    method: 'POST',
    headers,
    body: body === null ? undefined : JSON.stringify(body)
  });
  const text = await res.text();
  let json = null;
  if (text) { try { json = JSON.parse(text); } catch (e) { json = null; } }
  return { status: res.status, headers: res.headers, json, text };
}

function resultTypeOf(j) {
  return j && j.result && j.result.resultType;
}

async function main() {
  console.log(`== 2026-07-28 wire conformance verification against ${BASE} ==\n`);

  // ---------- 1. server/discover ----------
  console.log('[1] server/discover');
  {
    const r = await post({ jsonrpc: '2.0', id: 1, method: 'server/discover', params: {} });
    check('discover -> 200', r.status === 200, `got ${r.status}`);
    check('discover resultType=complete', resultTypeOf(r.json) === 'complete', `got ${resultTypeOf(r.json)}`);
    check('supportedVersions contains 2026-07-28', r.json && Array.isArray(r.json.result.supportedVersions) && r.json.result.supportedVersions.includes('2026-07-28'));
    check('capabilities present', r.json && r.json.result.capabilities && typeof r.json.result.capabilities === 'object');
    check('_meta.serverInfo present', r.json && r.json.result._meta && r.json.result._meta['io.modelcontextprotocol/serverInfo']);
    check('ttlMs present (CacheableResult)', r.json && typeof r.json.result.ttlMs === 'number');
    check('cacheScope present', r.json && typeof r.json.result.cacheScope === 'string');
    check('discover has no "result" key collision', r.json && !Array.isArray(r.json.result));
  }

  // ---------- 2. tools/list + pagination ----------
  console.log('\n[2] tools/list pagination');
  {
    const r1 = await post({ jsonrpc: '2.0', id: 2, method: 'tools/list', params: {} });
    check('tools/list page1 -> 200', r1.status === 200, `got ${r1.status}`);
    check('page1 resultType=complete', resultTypeOf(r1.json) === 'complete');
    check('page1 tools length=2', r1.json && r1.json.result.tools.length === 2, `got ${r1.json && r1.json.result.tools.length}`);
    check('page1 nextCursor=page_2', r1.json && r1.json.result.nextCursor === 'page_2');
    check('page1 ttlMs/cacheScope', r1.json && typeof r1.json.result.ttlMs === 'number' && typeof r1.json.result.cacheScope === 'string');

    const r2 = await post({ jsonrpc: '2.0', id: 3, method: 'tools/list', params: { cursor: 'page_2' } });
    check('page2 tools length=2 (remainder)', r2.json && r2.json.result.tools.length === 2, `got ${r2.json && r2.json.result.tools && r2.json.result.tools.length}`);
    check('page2 no nextCursor', r2.json && r2.json.result.nextCursor === undefined);
  }

  // ---------- 3. tools/call ----------
  console.log('\n[3] tools/call');
  {
    const r = await post({
      jsonrpc: '2.0', id: 4, method: 'tools/call',
      params: { name: 'calculate_add', arguments: { a: 2, b: 3 } }
    }, { 'Mcp-Name': 'calculate_add' });
    check('tools/call -> 200', r.status === 200, `got ${r.status}`);
    check('call resultType=complete', resultTypeOf(r.json) === 'complete');
    check('call content text contains sum', r.json && r.json.result.content && /5/.test(r.json.result.content[0].text), r.text);
  }

  // ---------- 4. MRTR (SEP-2322) ----------
  console.log('\n[4] MRTR approve_delete');
  {
    const r1 = await post({
      jsonrpc: '2.0', id: 5, method: 'tools/call',
      params: { name: 'approve_delete', arguments: { target: 'prod-db' } }
    }, { 'Mcp-Name': 'approve_delete' });
    check('first call -> 200', r1.status === 200, `got ${r1.status}`);
    check('first call resultType=input_required', resultTypeOf(r1.json) === 'input_required', `got ${resultTypeOf(r1.json)}`);
    check('inputRequests.confirm present', r1.json && r1.json.result.inputRequests && r1.json.result.inputRequests.confirm);
    check('inputRequests.confirm.method=elicitation/create', r1.json && r1.json.result.inputRequests.confirm.method === 'elicitation/create');
    check('requestState present', r1.json && typeof r1.json.result.requestState === 'string' && r1.json.result.requestState.length > 0);
    check('no resultType=complete on first call', resultTypeOf(r1.json) !== 'complete');

    const r2 = await post({
      jsonrpc: '2.0', id: 6, method: 'tools/call',
      params: {
        name: 'approve_delete',
        arguments: { target: 'prod-db' },
        inputResponses: { confirm: { action: 'accept', content: { confirm: true } } },
        requestState: r1.json.result.requestState
      }
    }, { 'Mcp-Name': 'approve_delete' });
    check('resend -> 200', r2.status === 200, `got ${r2.status}`);
    check('resend resultType=complete', resultTypeOf(r2.json) === 'complete', `got ${resultTypeOf(r2.json)}`);
    check('resend content indicates deleted', r2.json && /已删除/.test(r2.json.result.content[0].text), r2.text);
  }

  // ---------- 5. Header 校验 / 错误码 ----------
  console.log('\n[5] header validation & error codes');
  {
    // 5.1 缺 MCP-Protocol-Version header
    const raw1 = await fetch(BASE, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Mcp-Method': 'tools/list' },
      body: JSON.stringify({ jsonrpc: '2.0', id: 7, method: 'tools/list', params: {} })
    });
    const rj1 = await raw1.json();
    check('missing protocol header -> 400', raw1.status === 400, `got ${raw1.status}`);
    check('missing protocol header -> -32020', rj1.error && rj1.error.code === -32020, raw1.text);

    // 5.2 header 与 body _meta 版本不匹配
    const r2 = await post({
      jsonrpc: '2.0', id: 8, method: 'tools/list',
      params: { _meta: { 'io.modelcontextprotocol/protocolVersion': '2025-11-25' } }
    });
    check('meta mismatch -> 400', r2.status === 400, `got ${r2.status}`);
    check('meta mismatch -> -32020', r2.json && r2.json.error && r2.json.error.code === -32020, r2.text);

    // 5.3 版本不支持
    const r3 = await post({ jsonrpc: '2.0', id: 9, method: 'tools/list', params: {} }, { 'MCP-Protocol-Version': '2025-11-25' });
    check('unsupported version -> 400', r3.status === 400, `got ${r3.status}`);
    check('unsupported version -> -32022', r3.json && r3.json.error && r3.json.error.code === -32022, r3.text);
    check('unsupported version error.data.supported lists 2026-07-28',
      r3.json && r3.json.error.data && Array.isArray(r3.json.error.data.supported) && r3.json.error.data.supported.includes('2026-07-28'));
    check('unsupported version error.data.requested=2025-11-25',
      r3.json && r3.json.error.data && r3.json.error.data.requested === '2025-11-25');

    // 5.4 缺 Mcp-Method header（带上协议版本，排除缺协议头分支）
    const raw4 = await fetch(BASE, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'MCP-Protocol-Version': PROTOCOL_VERSION },
      body: JSON.stringify({ jsonrpc: '2.0', id: 10, method: 'tools/list', params: {} })
    });
    const r4j = await raw4.json();
    check('missing Mcp-Method -> 400', raw4.status === 400, `got ${raw4.status}`);
    check('missing Mcp-Method -> -32020', r4j.error && r4j.error.code === -32020, JSON.stringify(r4j));

    // 5.5 Mcp-Method 与 body 不匹配
    const r5 = await post({ jsonrpc: '2.0', id: 11, method: 'tools/list', params: {} }, { 'Mcp-Method': 'tools/call' });
    check('Mcp-Method mismatch -> -32020', r5.json && r5.json.error && r5.json.error.code === -32020, r5.text);

    // 5.6 tools/call 缺 Mcp-Name
    const r6 = await post({ jsonrpc: '2.0', id: 12, method: 'tools/call', params: { name: 'calculate_add', arguments: { a: 1, b: 2 } } });
    check('missing Mcp-Name -> -32020', r6.json && r6.json.error && r6.json.error.code === -32020, r6.text);

    // 5.7 Mcp-Name 与 body 不匹配
    const r7 = await post({ jsonrpc: '2.0', id: 13, method: 'tools/call', params: { name: 'calculate_add', arguments: { a: 1, b: 2 } } }, { 'Mcp-Name': 'get_system_time' });
    check('Mcp-Name mismatch -> -32020', r7.json && r7.json.error && r7.json.error.code === -32020, r7.text);

    // 5.8 未知方法 -> 404 + -32601
    const r8 = await post({ jsonrpc: '2.0', id: 14, method: 'unknown/method', params: {} });
    check('unknown method -> 404', r8.status === 404, `got ${r8.status}`);
    check('unknown method -> -32601', r8.json && r8.json.error && r8.json.error.code === -32601, r8.text);

    // 5.9 legacy initialize -> -32022
    const r9 = await post({ jsonrpc: '2.0', id: 15, method: 'initialize', params: { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 't', version: '1' } } });
    check('legacy initialize -> -32022', r9.json && r9.json.error && r9.json.error.code === -32022, r9.text);

    // 5.10 resources/read 不存在 -> -32602 (SEP-2164)
    const r10 = await post({ jsonrpc: '2.0', id: 16, method: 'resources/read', params: { uri: 'file:///nope.txt' } }, { 'Mcp-Name': 'file:///nope.txt' });
    check('missing resource -> -32602', r10.json && r10.json.error && r10.json.error.code === -32602, r10.text);

    // 5.11 resources/read 成功
    const r11 = await post({ jsonrpc: '2.0', id: 17, method: 'resources/read', params: { uri: 'file:///logs/system.log' } }, { 'Mcp-Name': 'file:///logs/system.log' });
    check('read resource -> complete', resultTypeOf(r11.json) === 'complete', r11.text);

    // 5.12 x-mcp-header 注解: approve_delete 带 region 缺 Mcp-Param-Region -> -32020
    const r12 = await post({ jsonrpc: '2.0', id: 18, method: 'tools/call', params: { name: 'approve_delete', arguments: { target: 'x', region: 'cn-east' } } }, { 'Mcp-Name': 'approve_delete' });
    check('missing Mcp-Param-Region -> -32020', r12.json && r12.json.error && r12.json.error.code === -32020, r12.text);
  }

  // ---------- 6. 通知 -> 202 无 body ----------
  console.log('\n[6] notification');
  {
    const r = await post({ jsonrpc: '2.0', method: 'notifications/cancelled', params: { requestId: 999, reason: 'test' } });
    check('notification -> 202', r.status === 202, `got ${r.status}`);
    check('notification -> no body', r.text === '', `got body: ${r.text.slice(0, 80)}`);
  }

  // ---------- 7. GET / DELETE -> 405 ----------
  console.log('\n[7] GET/DELETE rejected');
  {
    const g = await fetch(BASE, { method: 'GET' });
    check('GET -> 405', g.status === 405, `got ${g.status}`);
    const d = await fetch(BASE, { method: 'DELETE' });
    check('DELETE -> 405', d.status === 405, `got ${d.status}`);
  }

  console.log(`\n== Result: ${passed} passed, ${failed} failed ==`);
  process.exit(failed === 0 ? 0 : 1);
}

main().catch(err => {
  console.error('Verification crashed:', err);
  process.exit(1);
});
