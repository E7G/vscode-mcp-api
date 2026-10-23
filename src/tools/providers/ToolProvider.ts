import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'

export interface ToolContext {
  workspaceRoot?: string
  bridge?: unknown
  settings?: unknown
  terminalManager?: unknown
}

export interface ToolProvider {
  name: string
  register(server: McpServer, context: ToolContext): void | Promise<void>
}
