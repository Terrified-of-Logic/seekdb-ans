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

#define USING_LOG_PREFIX SHARE
#include "share/ai_column/ob_ai_column_metadata_util.h"
#include "share/ai_column/ob_ai_column_define.h"
#include "share/ai_column/ob_ai_column_proxy.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "common/mysqlclient/ob_isql_client.h"

namespace oceanbase
{
namespace share
{

int ObAIColumnMetadataUtil::get_hidden_column_id(const schema::ObTableSchema &table_schema,
                                                 const char *prefix,
                                                 const common::ObString &result_column_name,
                                                 int64_t &column_id)
{
  int ret = OB_SUCCESS;
  char name_buf[512];
  int64_t name_len = 0;
  column_id = ObAIColumnSchema::INVALID_ID;
  if (OB_ISNULL(prefix) || result_column_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(prefix), K(result_column_name));
  } else {
    name_len = snprintf(name_buf, sizeof(name_buf), "%s%.*s", prefix,
                        result_column_name.length(), result_column_name.ptr());
  }
  if (OB_FAIL(ret)) {
  } else if (OB_UNLIKELY(name_len <= 0 || name_len >= static_cast<int64_t>(sizeof(name_buf)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hidden column name too long", K(ret), K(name_len), K(result_column_name));
  } else {
    const schema::ObColumnSchemaV2 *column = nullptr;
    const common::ObString hidden_name(static_cast<int32_t>(name_len), name_buf);
    if (OB_ISNULL(column = table_schema.get_column_schema(hidden_name))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_INFO("[aicol] hidden column NOT found", K(hidden_name),
               K(table_schema.get_table_name_str()), K(table_schema.get_column_count()));
      for (int64_t i = 0; i < table_schema.get_column_count(); ++i) {
        const schema::ObColumnSchemaV2 *c = table_schema.get_column_schema_by_idx(i);
        if (OB_NOT_NULL(c)) {
          LOG_INFO("[aicol] table has column", K(i), K(c->get_column_name_str()),
                   K(c->get_column_id()), K(c->is_hidden()));
        }
      }
    } else {
      column_id = static_cast<int64_t>(column->get_column_id());
    }
  }
  return ret;
}

int ObAIColumnMetadataUtil::fill_hidden_column_ids(const schema::ObTableSchema &table_schema,
                                                   const int64_t table_id,
                                                   common::ObIArray<ObAIColumnSchema> &ai_columns)
{
  int ret = OB_SUCCESS;
  LOG_INFO("[aicol] fill_hidden_column_ids begin", K(table_id), K(ai_columns.count()),
           K(table_schema.get_table_name_str()), K(table_schema.get_column_count()));
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    ObAIColumnSchema &def = ai_columns.at(i);
    def.table_id_ = table_id;
    if (OB_FAIL(get_hidden_column_id(table_schema, OB_AI_COLUMN_STATE_PREFIX,
                                     def.column_name_, def.state_column_id_))) {
    } else if (OB_FAIL(get_hidden_column_id(table_schema, OB_AI_COLUMN_REQUEST_PREFIX,
                                            def.column_name_, def.request_column_id_))) {
    } else if (OB_FAIL(get_hidden_column_id(table_schema, OB_AI_COLUMN_RESULT_SPEC_PREFIX,
                                            def.column_name_, def.result_spec_column_id_))) {
    } else {
      LOG_INFO("[aicol] fill hidden ids done", K(ret), K(def.column_name_),
               K(def.column_id_), K(def.state_column_id_), K(def.request_column_id_),
               K(def.result_spec_column_id_));
    }
  }
  return ret;
}

int ObAIColumnMetadataUtil::resolve_columns_by_name(const schema::ObTableSchema &table_schema,
                                                    common::ObIArray<ObAIColumnSchema> &ai_columns)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    ObAIColumnSchema &def = ai_columns.at(i);
    if (ObAIColumnSchema::INVALID_ID == def.column_id_) {
      const schema::ObColumnSchemaV2 *column = nullptr;
      if (OB_ISNULL(column = table_schema.get_column_schema(def.column_name_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column result column not found", K(ret), K(def.column_name_),
                 K(table_schema.get_table_name_str()));
      } else {
        def.column_id_ = static_cast<int64_t>(column->get_column_id());
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(get_hidden_column_id(table_schema, OB_AI_COLUMN_STATE_PREFIX,
                                            def.column_name_, def.state_column_id_))) {
    } else if (OB_FAIL(get_hidden_column_id(table_schema, OB_AI_COLUMN_REQUEST_PREFIX,
                                            def.column_name_, def.request_column_id_))) {
    } else if (OB_FAIL(get_hidden_column_id(table_schema, OB_AI_COLUMN_RESULT_SPEC_PREFIX,
                                            def.column_name_, def.result_spec_column_id_))) {
    } else {
      LOG_INFO("[aicol] fill hidden ids done", K(ret), K(def.column_name_),
               K(def.column_id_), K(def.state_column_id_), K(def.request_column_id_),
               K(def.result_spec_column_id_));
    }
  }
  return ret;
}

int ObAIColumnMetadataUtil::write_ai_columns(common::ObMySQLTransaction &trans,
                                             const common::ObIArray<ObAIColumnSchema> &ai_columns)
{
  int ret = OB_SUCCESS;
  LOG_INFO("[aicol] write_ai_columns begin", K(ai_columns.count()));
  if (ai_columns.empty()) {
    // nothing to write
  } else if (OB_FAIL(ObAIColumnProxy::insert_ai_columns(trans, ai_columns))) {
    LOG_WARN("failed to insert ai column metadata", K(ret), K(ai_columns.count()));
  }
  return ret;
}

int ObAIColumnMetadataUtil::cleanup_dropped_ai_columns(
    common::ObMySQLTransaction &trans,
    common::ObISQLClient &sql_proxy,
    const schema::ObTableSchema &new_table_schema)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  ObSEArray<ObAIColumnSchema, AI_COLUMN_DEF_ARRAY_SIZE> ai_columns;
  const int64_t table_id = new_table_schema.get_table_id();
  if (OB_UNLIKELY(table_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid table id", KR(ret), K(table_id));
  } else if (OB_FAIL(ObAIColumnProxy::select_ai_columns_by_table(
                 allocator, sql_proxy, table_id, ai_columns))) {
    LOG_WARN("fail to select ai columns", KR(ret), K(table_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const ObAIColumnSchema &ai_column = ai_columns.at(i);
      if (NULL == new_table_schema.get_column_schema(ai_column.column_name_)) {
        // The result column was dropped: clean metadata and pending tasks.
        if (OB_FAIL(ObAIColumnProxy::delete_ai_column_tasks_by_column(
                trans, table_id, ai_column.column_id_))) {
        } else if (OB_FAIL(ObAIColumnProxy::delete_ai_column(
                       trans, table_id, ai_column.column_id_))) {
        } else {
          LOG_INFO("[aicol] dropped ai column metadata cleaned",
                   K(table_id), K(ai_column.column_id_), K(ai_column.column_name_));
        }
      }
    }
  }
  return ret;
}

int ObAIColumnMetadataUtil::delete_ai_columns(common::ObMySQLTransaction &trans,
                                              const int64_t table_id)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObAIColumnProxy::delete_ai_columns_by_table(trans, table_id))) {
    LOG_WARN("failed to delete ai column metadata", K(ret), K(table_id));
  } else if (OB_FAIL(ObAIColumnProxy::delete_ai_column_tasks_by_table(trans, table_id))) {
    LOG_WARN("failed to delete ai column tasks", K(ret), K(table_id));
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
