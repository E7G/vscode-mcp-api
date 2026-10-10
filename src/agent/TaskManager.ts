import { randomUUID } from 'node:crypto'

export type TaskState =
  | 'created'
  | 'running'
  | 'waiting_approval'
  | 'completed'
  | 'failed'
  | 'rolled_back'

export interface AgentTask {
  id: string
  goal: string
  state: TaskState
  createdAt: number
  updatedAt: number
  steps: string[]
  logs: string[]
  snapshot?: string
  error?: string
}

/**
 * MCP Agent task lifecycle manager.
 * Keeps long running agent jobs independent from MCP request lifetime.
 */
export class TaskManager {
  private readonly tasks = new Map<string, AgentTask>()

  create(goal: string, steps: string[] = []): AgentTask {
    const now = Date.now()
    const task: AgentTask = {
      id: randomUUID(),
      goal,
      state: 'created',
      createdAt: now,
      updatedAt: now,
      steps,
      logs: [],
    }
    this.tasks.set(task.id, task)
    return task
  }

  get(id: string): AgentTask | undefined {
    return this.tasks.get(id)
  }

  update(id: string, patch: Partial<AgentTask>): AgentTask {
    const task = this.tasks.get(id)
    if (!task) throw new Error(`Unknown task: ${id}`)
    Object.assign(task, patch, { updatedAt: Date.now() })
    return task
  }

  log(id: string, message: string): AgentTask {
    const task = this.tasks.get(id)
    if (!task) throw new Error(`Unknown task: ${id}`)
    task.logs.push(`[${new Date().toISOString()}] ${message}`)
    task.updatedAt = Date.now()
    return task
  }

  setSnapshot(id: string, snapshot: string): AgentTask {
    return this.update(id, { snapshot })
  }

  list(): AgentTask[] {
    return [...this.tasks.values()]
  }

  rollback(id: string): AgentTask {
    return this.update(id, { state: 'rolled_back' })
  }
}
