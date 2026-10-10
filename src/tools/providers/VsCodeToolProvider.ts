import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'
import type { ToolProvider, ToolContext } from './ToolProvider.js'

/**
 * VS Code specific tools: editor state, LSP, diff and commands.
 */
export class VsCodeToolProvider implements ToolProvider {
  name = 'vscode'

  register(_server: McpServer, _context: ToolContext): void {
    // Existing VS Code bridge tools will be migrated here.
  }
}
