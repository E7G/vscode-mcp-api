import { randomUUID } from 'node:crypto'

export interface PatchSnapshot {
  id: string
  taskId: string
  filePath: string
  before: string
  after?: string
  createdAt: number
}

/**
 * Stores reversible file modifications for MCP Agent operations.
 */
export class PatchManager {
  private readonly snapshots = new Map<string, PatchSnapshot>()

  create(taskId: string, filePath: string, before: string, after?: string): PatchSnapshot {
    const snapshot: PatchSnapshot = {
      id: randomUUID(),
      taskId,
      filePath,
      before,
      after,
      createdAt: Date.now(),
    }
    this.snapshots.set(snapshot.id, snapshot)
    return snapshot
  }

  get(id: string): PatchSnapshot | undefined {
    return this.snapshots.get(id)
  }

  list(taskId?: string): PatchSnapshot[] {
    const values = [...this.snapshots.values()]
    return taskId ? values.filter(item => item.taskId === taskId) : values
  }
}
