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

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_DEFINE_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_DEFINE_H_

#include "share/ai_column/ob_ai_column_struct.h"

namespace oceanbase
{
namespace share
{

// ---------- hidden column name prefixes (Lead frozen v1) ----------
constexpr const char *OB_AI_COLUMN_STATE_PREFIX = "__ai_state_";
constexpr const char *OB_AI_COLUMN_REQUEST_PREFIX = "__ai_request_";
constexpr const char *OB_AI_COLUMN_RESULT_SPEC_PREFIX = "__ai_result_spec_";

// ---------- inner table names ----------
// NOTE: use the generated constants header instead
//   #include "share/inner_table/ob_inner_table_schema_constants.h"
//   OB_ALL_AI_COLUMN_TNAME / OB_ALL_AI_COLUMN_TASK_TNAME / OB_DBA_OB_AI_COLUMNS_TNAME ...
// Do NOT redefine them here (single-tenant fork: no tenant_id column on inner tables).

// ---------- WITH options ----------
constexpr const char *OB_AI_COLUMN_SYNC_MODE = "sync_mode";
constexpr const char *OB_AI_COLUMN_MAX_RETRIES = "max_retries";
constexpr const char *OB_AI_COLUMN_DEFAULT_SYNC_MODE = "async";
constexpr int64_t OB_AI_COLUMN_DEFAULT_MAX_RETRIES = 0;
constexpr int64_t OB_AI_COLUMN_MAX_RETRIES_LIMIT = 100; // sanity bound for WITH value

// ---------- hidden state column JSON format (Lead frozen v1) ----------
// JSON object: {"state":"READY"} ; ObAIColumnRowState::get_state_str maps enum->string.
constexpr const char *OB_AI_COLUMN_STATE_JSON_KEY = "state";
constexpr const char *OB_AI_COLUMN_REQUEST_JSON_KEY = "request";

// ---------- retry statement pseudo column ----------
// `status` (unquoted) is the AI column row state pseudo column; `\`status\`` is the
// physical user column. See contract.md §13.6.
constexpr const char *OB_AI_COLUMN_STATUS_PSEUDO_COLUMN = "status";

// ---------- strict read system variable ----------
constexpr const char *OB_AI_COLUMN_STRICT_READ_SYSVAR = "ai_column_strict_read";

// ---------- normalized prompt wrapper (must contain this for json result columns) ----------
constexpr const char *OB_AI_COLUMN_JSON_SYSTEM_PROMPT =
    "Return exactly one valid JSON value";
constexpr const char *OB_AI_COLUMN_INT_SYSTEM_PROMPT =
    "Return exactly one base-10 integer";

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_DEFINE_H_
