#!/bin/sh
# btdd-port pre-commit: block any commit that contains ROM content (docs/ROM_POLICY.md).
# Install: cp tools/pre-commit-hook.sh .git/hooks/pre-commit
cd "$(git rev-parse --show-toplevel)" || exit 1
python -I tools/release_audit.py --staged
status=$?
if [ $status -ne 0 ]; then
    echo "COMMIT BLOCKED: release_audit.py found ROM content or could not run (see above)."
    exit 1
fi
