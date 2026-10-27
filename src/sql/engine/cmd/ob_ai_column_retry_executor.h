/**
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_SQL_ENGINE_CMD_OB_AI_COLUMN_RETRY_EXECUTOR_H_
#define OCEANBASE_SQL_ENGINE_CMD_OB_AI_COLUMN_RETRY_EXECUTOR_H_

#include "lib/ob_define.h"

namespace oceanbase
{
namespace sql
{
class ObExecContext;
class ObAlterTableStmt;

// Executes the data part of `ALTER TABLE t RETRY AI COLUMN <col> [WHERE <pred>]`:
// for every row selected by the predicate, the AI result column is invalidated
// (set to NULL) and a new task row is created in the same transaction.
// No schema change is involved, so the normal ALTER TABLE DDL flow is skipped.
class ObAIColumnRetryExecutor
{
public:
  static int execute(ObExecContext &ctx, ObAlterTableStmt &stmt);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_CMD_OB_AI_COLUMN_RETRY_EXECUTOR_H_
