# AGENTS.md

本仓库的权威约束在 `CLAUDE.md`，任何 agent 开始工作前先完整阅读它。
`.claude/rules/*.md` 由 Claude Code 按 `paths:` 自动加载，其他 agent 需按 CLAUDE.md「架构规则」表手动读取。
`.claude/skills/*/SKILL.md` 是流程说明，可直接照做；`.claude/agents/` 是 Claude Code 子 Agent 定义。

环境：shell 是 git-bash；Python 用 `python`（不是 `python3`）。
