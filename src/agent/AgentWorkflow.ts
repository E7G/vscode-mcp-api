import * as vscode from 'vscode'

export type AgentPlanStepType =
  | 'analyze'
  | 'read'
  | 'patch'
  | 'verify'
  | 'rollback'

export interface AgentPlanStep {
  id: string
  type: AgentPlanStepType
  title: string
  description: string
  risk: 'low' | 'medium' | 'high'
  requiresApproval: boolean
}

export interface AgentPlan {
  goal: string
  createdAt: number
  steps: AgentPlanStep[]
}

export interface AgentExecutionResult {
  success: boolean
  stepId: string
  message: string
  data?: unknown
}

/**
 * Agent workflow foundation.
 *
 * The MCP tools layer can use this class to implement:
 * - analyze_project
 * - prepare_patch
 * - apply_patch
 * - verify_change
 *
 * Execution is intentionally approval-aware: destructive operations never
 * happen implicitly.
 */
export class AgentWorkflow {
  private plans = new Map<string, AgentPlan>()

  createPlan(goal: string, steps: AgentPlanStep[]): AgentPlan {
    const plan: AgentPlan = {
      goal,
      createdAt: Date.now(),
      steps,
    }
    this.plans.set(this.id(plan), plan)
    return plan
  }

  getPlan(id: string): AgentPlan | undefined {
    return this.plans.get(id)
  }

  listPlans(): AgentPlan[] {
    return [...this.plans.values()]
  }

  async executeStep(step: AgentPlanStep, approved = false): Promise<AgentExecutionResult> {
    if (step.requiresApproval && !approved) {
      return {
        success: false,
        stepId: step.id,
        message: 'This step requires user approval before execution.',
      }
    }

    switch (step.type) {
      case 'analyze':
        return {
          success: true,
          stepId: step.id,
          message: 'Analysis phase completed.',
        }
      case 'read':
        return {
          success: true,
          stepId: step.id,
          message: 'Read phase completed.',
        }
      case 'patch':
        return {
          success: true,
          stepId: step.id,
          message: 'Patch prepared. Applying requires explicit approval.',
        }
      case 'verify':
        return {
          success: true,
          stepId: step.id,
          message: 'Verification phase completed.',
        }
      case 'rollback':
        return {
          success: true,
          stepId: step.id,
          message: 'Rollback phase completed.',
        }
    }
  }

  private id(plan: AgentPlan): string {
    return `${plan.createdAt}-${plan.goal.slice(0, 32)}`
  }
}

export function createDefaultWorkflow(goal: string): AgentPlan {
  return {
    goal,
    createdAt: Date.now(),
    steps: [
      {
        id: 'analyze',
        type: 'analyze',
        title: 'Analyze project',
        description: 'Inspect workspace structure, dependencies and diagnostics.',
        risk: 'low',
        requiresApproval: false,
      },
      {
        id: 'prepare_patch',
        type: 'patch',
        title: 'Prepare patch',
        description: 'Generate a reviewable change set.',
        risk: 'medium',
        requiresApproval: true,
      },
      {
        id: 'verify',
        type: 'verify',
        title: 'Verify changes',
        description: 'Run diagnostics and validation.',
        risk: 'low',
        requiresApproval: false,
      },
    ],
  }
}
