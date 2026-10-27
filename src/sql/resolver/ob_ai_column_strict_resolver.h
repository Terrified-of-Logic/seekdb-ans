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

#ifndef OCEANBASE_SQL_RESOLVER_OB_AI_COLUMN_STRICT_RESOLVER_H_
#define OCEANBASE_SQL_RESOLVER_OB_AI_COLUMN_STRICT_RESOLVER_H_

#include "lib/ob_define.h"
#include "lib/container/ob_iarray.h"

namespace oceanbase
{
namespace sql
{
class ObResolverParams;
class ObDMLStmt;
class ObSqlCtx;

// Strict read/write check for AI columns (`ai_column_strict_read=on`).
//
// A statement that references an AI result column (in projection, where clause or
// any subquery) is rejected with OB_AI_COLUMN_UNFINISHED_TASK when the AI column is
// not fully ready (column wide: any row whose result is NULL, or any leftover task).
class ObAIColumnStrictResolver
{
public:
  struct AIColumnRef
  {
    AIColumnRef() : table_id_(common::OB_INVALID_ID), column_id_(common::OB_INVALID_ID) {}
    AIColumnRef(const uint64_t table_id, const uint64_t column_id)
      : table_id_(table_id), column_id_(column_id) {}
    uint64_t table_id_;
    uint64_t column_id_;
  };

  static int check_strict_read(ObResolverParams &params, ObDMLStmt &stmt);
  static int collect_refs(ObDMLStmt &stmt, common::ObIArray<AIColumnRef> &refs);
private:
  static int check_one_column(ObResolverParams &params,
                              ObSqlCtx *sql_ctx,
                              const uint64_t table_id,
                              const uint64_t column_id);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_OB_AI_COLUMN_STRICT_RESOLVER_H_
