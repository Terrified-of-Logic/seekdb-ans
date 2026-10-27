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
#include "share/ai_column/ob_ai_column_struct.h"

namespace oceanbase
{
namespace share
{

OB_SERIALIZE_MEMBER_SIMPLE(ObAIColumnSchema,
                           table_id_,
                           column_id_,
                           column_name_,
                           normalized_expr_,
                           sync_mode_,
                           system_prompt_,
                           dep_columns_,
                           ai_model_name_,
                           user_expr_,
                           max_retries_,
                           state_column_id_,
                           request_column_id_,
                           result_spec_column_id_);

} // namespace share
} // namespace oceanbase
