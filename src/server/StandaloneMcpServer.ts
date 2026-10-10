import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js'
import { z } from 'zod'

/**
 * Standalone MCP transport entry.
 *
 * This is intentionally separated from the VS Code extension runtime. The goal
 * is to allow a future Windows executable wrapper to expose the same MCP
 * protocol without requiring an editor UI.
 */
export async function startStandaloneMcpServer(): Promise<void> {
  const server = new McpServer({
    name: 'vscode-mcp-api-server',
    version: '1.0.0',
  })

  server.tool(
    'server_info',
    '返回 MCP Server 基本信息',
    {},
    async () => ({
      content: [{
        type: 'text',
        text: JSON.stringify({
          name: 'vscode-mcp-api-server',
          mode: 'standalone',
          status: 'running',
        }),
      }],
    }),
  )

  const transport = new StdioServerTransport()
  await server.connect(transport)
}
