import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'
import { z } from 'zod'
import { AgentWorkflow, createDefaultWorkflow } from '../../agent/AgentWorkflow.js'
import type { ToolProvider, ToolContext } from './ToolProvider.js'

/**
 * Read-only planning provider. Preparing a patch here only returns a proposed
 * step requiring approval; it never claims to have generated or applied a diff.
 */
export class AgentToolProvider implements ToolProvider {
  readonly name = 'agent-workflow'
  private readonly workflow = new AgentWorkflow()

  register(server: McpServer, _context: ToolContext): void {
    server.tool(
      'agent_create_plan',
      '根据目标创建需审批的开发计划',
      { goal: z.string().min(1).describe('开发目标或问题描述') },
      async ({ goal }) => {
        const plan = this.workflow.createPlan(goal, createDefaultWorkflow(goal).steps)
        const taskId = `${plan.createdAt}-${plan.goal.slice(0, 32)}`
        return { content: [{ type: 'text', text: JSON.stringify({ taskId, plan }) }] }
      },
    )

    server.tool(
      'agent_task_status',
      '查询此前创建的 Agent 开发计划',
      { taskId: z.string() },
      async ({ taskId }) => {
        const plan = this.workflow.getPlan(taskId)
        if (!plan) {
          return {
            isError: true,
            content: [{ type: 'text', text: `Unknown plan: ${taskId}` }],
          }
        }
        return { content: [{ type: 'text', text: JSON.stringify({ taskId, plan }) }] }
      },
    )

    server.tool(
      'agent_prepare_patch',
      '查看待审批的修改步骤，不修改磁盘或伪造补丁',
      { taskId: z.string() },
      async ({ taskId }) => {
        const plan = this.workflow.getPlan(taskId)
        if (!plan) {
          return {
            isError: true,
            content: [{ type: 'text', text: `Unknown plan: ${taskId}` }],
          }
        }
        return {
          content: [{
            type: 'text',
            text: JSON.stringify({
              taskId,
              prepared: false,
              requiresApproval: true,
              steps: plan.steps.filter(step => step.type === 'patch'),
              message: 'This is a reviewable plan, not a generated file patch.',
            }),
          }],
        }
      },
    )
  }
}
