import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'

export interface ToolProviderContext {
  mode: 'vscode' | 'standalone'
  workspace?: string
  bridge?: unknown
  settings?: unknown
  terminalManager?: unknown
}

export type ToolProvider = (server: McpServer, context: ToolProviderContext) => void | Promise<void>

/**
 * Central registry for MCP tools.
 *
 * The MCP server should not care whether tools are backed by VS Code,
 * filesystem, or remote workspace. Providers can be swapped without
 * changing transport code.
 */
export class ToolRegistry {
  private providers: ToolProvider[] = []

  register(provider: ToolProvider): this {
    this.providers.push(provider)
    return this
  }

  async mount(server: McpServer, context: ToolProviderContext): Promise<void> {
    for (const provider of this.providers) {
      await provider(server, context)
    }
  }

  count(): number {
    return this.providers.length
  }
}

export function createToolRegistry(): ToolRegistry {
  return new ToolRegistry()
}
