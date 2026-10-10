import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'
import { z } from 'zod'
import { TaskManager } from '../../agent/TaskManager.js'
import { PatchManager } from '../../agent/PatchManager.js'

export class AgentExecutionProvider {
  constructor(
    private readonly tasks: TaskManager,
    private readonly patches: PatchManager,
  ) {}

  register(server: McpServer): void {
    server.tool(
      'agent_start_task',
      '创建一个新的 Agent 长任务',
      { goal: z.string() },
      async ({ goal }) => {
        const task = this.tasks.create(goal, [
          'analyze_project',
          'prepare_patch',
          'verify_change',
        ])
        return { content: [{ type: 'text', text: JSON.stringify(task) }] }
      },
    )

    server.tool(
      'agent_task_status',
      '查询 Agent 任务状态',
      { taskId: z.string() },
      async ({ taskId }) => {
        const task = this.tasks.get(taskId)
        if (!task) throw new Error(`Task not found: ${taskId}`)
        return { content: [{ type: 'text', text: JSON.stringify(task) }] }
      },
    )

    server.tool(
      'agent_create_snapshot',
      '保存修改前快照，用于回滚',
      {
        taskId: z.string(),
        filePath: z.string(),
        before: z.string(),
      },
      async ({ taskId, filePath, before }) => {
        const snapshot = this.patches.create(taskId, filePath, before)
        this.tasks.setSnapshot(taskId, snapshot.id)
        return { content: [{ type: 'text', text: JSON.stringify(snapshot) }] }
      },
    )

    server.tool(
      'agent_rollback',
      '标记任务回滚并恢复流程状态',
      { taskId: z.string() },
      async ({ taskId }) => {
        const task = this.tasks.rollback(taskId)
        return { content: [{ type: 'text', text: JSON.stringify(task) }] }
      },
    )
  }
}
