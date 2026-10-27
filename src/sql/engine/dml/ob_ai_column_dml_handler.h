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

#ifndef OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_DML_HANDLER_H_
#define OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_DML_HANDLER_H_

#include "lib/ob_define.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
} // namespace schema
} // namespace share

namespace sql
{

class ObExecContext;
struct ObDMLModifyRowNode;

// Per-row maintenance of the AI column tasks for INSERT/UPDATE/DELETE.
// Invoked from ObDMLService after the row has been written (same stage as AFTER
// ROW triggers), so that the task table writes join the current DML transaction.
class ObAIColumnDMLHandler
{
public:
  static int handle_after_row(ObDMLModifyRowNode &modify_row);
private:
  static int handle_insert(ObDMLModifyRowNode &modify_row);
  static int handle_update(ObDMLModifyRowNode &modify_row);
  static int handle_delete(ObDMLModifyRowNode &modify_row);
  static int get_table_schema(ObExecContext &exec_ctx,
                              const int64_t table_id,
                              const share::schema::ObTableSchema *&table_schema);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_DML_HANDLER_H_
