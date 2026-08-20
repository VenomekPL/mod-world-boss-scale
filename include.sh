#!/usr/bin/env bash
# mod-world-boss-scale — world SQL under data/sql/db-world/ is a load marker only.
MOD_WORLD_BOSS_SCALE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DB_WORLD_CUSTOM_PATHS+=(
    "$MOD_WORLD_BOSS_SCALE_ROOT/data/sql/db-world/"
)
