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

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_METADATA_UTIL_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_METADATA_UTIL_H_

#include "lib/ob_define.h"
#include "lib/container/ob_se_array.h"
#include "share/ai_column/ob_ai_column_struct.h"
#include "common/mysqlclient/ob_mysql_transaction.h"

namespace oceanbase
{
namespace share
{

namespace schema
{
class ObTableSchema;
}

// Helper for rootserver DDL paths to write/delete __all_ai_column metadata.
class ObAIColumnMetadataUtil
{
public:
  static const int64_t AI_COLUMN_DEF_ARRAY_SIZE = 4;

  // CREATE TABLE: fill table_id and resolve 3 hidden column ids by column name.
  static int fill_hidden_column_ids(const schema::ObTableSchema &table_schema,
                                    const int64_t table_id,
                                    common::ObIArray<ObAIColumnSchema> &ai_columns);

  // ALTER TABLE ADD COLUMN: result column id may be invalid(-1); resolve it and
  // the 3 hidden column ids by column name from the new table schema.
  static int resolve_columns_by_name(const schema::ObTableSchema &table_schema,
                                     common::ObIArray<ObAIColumnSchema> &ai_columns);

  // Write metadata rows into __all_ai_column inside the DDL transaction.
  static int write_ai_columns(common::ObMySQLTransaction &trans,
                              const common::ObIArray<ObAIColumnSchema> &ai_columns);

  // DROP TABLE: delete metadata rows and task rows of the table.
  static int delete_ai_columns(common::ObMySQLTransaction &trans,
                               const int64_t table_id);

  // ALTER TABLE ... DROP COLUMN: delete metadata/task rows of AI columns that no
  // longer exist in the new table schema. Must run inside the DDL transaction.
  static int cleanup_dropped_ai_columns(common::ObMySQLTransaction &trans,
                                        common::ObISQLClient &sql_proxy,
                                        const schema::ObTableSchema &new_table_schema);

private:
  static int get_hidden_column_id(const schema::ObTableSchema &table_schema,
                                  const char *prefix,
                                  const common::ObString &result_column_name,
                                  int64_t &column_id);
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_METADATA_UTIL_H_
