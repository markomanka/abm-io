#!/usr/bin/env bash
# Replaces the OWNER/REPO placeholders with your GitHub account and repository name.
# Usage: scripts/init-repo.sh <github-username> [repository-name]
set -euo pipefail
if [ $# -lt 1 ]; then echo "usage: $0 <github-username> [repository-name]" >&2; exit 2; fi
owner="$1"; repo="${2:-abm-io}"
cd "$(dirname "$0")/.."
files=(README.md SECURITY.md CONTRIBUTING.md .github/CODEOWNERS .github/ISSUE_TEMPLATE/config.yml docs/ROADMAP.md docs/QUALITY-GATES.md)
for f in "${files[@]}"; do
  [ -f "$f" ] && OWNER_NEW="$owner" REPO_NEW="$repo" perl -pi -e 's/\bOWNER\b/$ENV{OWNER_NEW}/g; s/\bREPO\b/$ENV{REPO_NEW}/g' "$f"
done
echo "placeholders set to $owner/$repo in: ${files[*]}"
