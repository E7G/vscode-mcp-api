import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'
import { z } from 'zod'
import { AgentWorkflow } from '../../agent/AgentWorkflow.js'
import type { ToolProvider } from './ToolProvider.js'

/**
 * Agent tools exposed through MCP.
 * This layer intentionally stays protocol focused; execution policy lives in AgentWorkflow.
 */
export class AgentToolProvider implements ToolProvider {
  private workflow = new AgentWorkflow()

  register(server: McpServer): void {
    server.tool(
      'agent_create_plan',
      '根据目标创建开发任务计划',
      {
        goal: z.string().describe('开发目标或问题描述'),
      },
      async ({ goal }) => {
        const plan = this.workflow.createPlan(goal)
        return { content: [{ type: 'text', text: JSON.stringify(plan) }] }
      },
    )

    server.tool(
      'agent_task_status',
      '获取 Agent 当前任务状态',
      {
        taskId: z.string(),
      },
      async ({ taskId }) => {
        const status = this.workflow.getStatus(taskId)
        return { content: [{ type: 'text', text: JSON.stringify(status) }] }
      },
    )

    server.tool(
      'agent_prepare_patch',
      '生成修改计划，不直接修改文件',
      {
        taskId: z.string(),
      },
      async ({ taskId }) => {
        const patch = this.workflow.preparePatch(taskId)
        return { content: [{ type: 'text', text: JSON.stringify(patch) }] }
      },
    )
  }
}
