import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'
import type { ToolProvider, ToolContext } from './ToolProvider.js'

/**
 * Tools that do not require VS Code runtime.
 * This provider is used by standalone MCP server builds.
 */
export class CommonToolProvider implements ToolProvider {
  name = 'common'

  register(_server: McpServer, _context: ToolContext): void {
    // File/Git/Terminal tools will be migrated here incrementally.
  }
}
