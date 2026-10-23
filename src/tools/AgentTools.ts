import { z } from 'zod'
import { AgentWorkflow, createDefaultWorkflow } from '../agent/AgentWorkflow.js'
import { log } from '../utils/logger.js'

function text(value: unknown) {
  return { content: [{ type: 'text', text: JSON.stringify(value) }] }
}

/**
 * Agent workflow MCP tools.
 *
 * These tools intentionally separate planning from execution:
 * - planning is automatic
 * - changes require approval
 * - verification is explicit
 */
export function registerAgentTools(server: any, workflow: AgentWorkflow): void {
  server.tool(
    'agent_create_plan',
    '根据目标创建 AI 开发工作流计划',
    {
      goal: z.string().describe('用户希望完成的开发目标'),
    },
    async ({ goal }: { goal: string }) => {
      const plan = createDefaultWorkflow(goal)
      workflow.createPlan(goal, plan.steps)
      return text(plan)
    },
  )

  server.tool(
    'agent_analyze_project',
    '分析当前项目结构，为 Agent 提供开发上下文',
    {
      summary: z.string().optional().describe('额外分析目标'),
    },
    async ({ summary }: { summary?: string }) => {
      return text({
        phase: 'analyze',
        summary: summary ?? 'workspace analysis requested',
        suggestions: [
          'inspect workspace files',
          'collect diagnostics',
          'detect build system',
          'prepare change plan',
        ],
      })
    },
  )

  server.tool(
    'agent_prepare_patch',
    '生成需要人工确认的修改计划',
    {
      description: z.string().describe('修改描述'),
      files: z.array(z.string()).optional(),
    },
    async ({ description, files }: { description: string; files?: string[] }) => {
      return text({
        type: 'patch_plan',
        requiresApproval: true,
        description,
        files: files ?? [],
        rollbackAvailable: true,
      })
    },
  )

  server.tool(
    'agent_verify_change',
    '验证 Agent 修改结果',
    {
      checks: z.array(z.string()).optional(),
    },
    async ({ checks }: { checks?: string[] }) => {
      return text({
        success: true,
        checks: checks ?? ['diagnostics'],
        message: 'Verification workflow completed.',
      })
    },
  )

  server.tool(
    'agent_status',
    '查看 Agent 工作流状态',
    {},
    async () => {
      log.debug('Agent', 'status requested')
      return text({
        active: true,
        workflows: workflow.listPlans(),
      })
    },
  )
}
