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

#ifndef OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_AI_EXECUTOR_H_
#define OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_AI_EXECUTOR_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "share/ai_column/ob_ai_column_struct.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
}
namespace share
{
namespace schema
{
class ObSchemaGetterGuard;
}
}
namespace observer
{

// Executes the AI request of one AI column expression, WITHOUT going through the
// SQL expression engine (so the typed system prompt can be sent as a real system
// message). Reuses ObAIFuncUtils' provider-level helpers + ObAIFuncClient; the
// AI function implementation files are untouched.
class ObAIColumnAIExecutor
{
public:
  // user_content: evaluated prompt/content expression of the AI column for one row.
  // result: completion -> text/JSON string; embedding -> JSON float array text
  //         (e.g. "[1.0,2.0,3.0,4.0]", directly usable as a vector literal).
  static int execute(common::ObIAllocator &allocator,
                     share::schema::ObSchemaGetterGuard &schema_guard,
                     const share::ObAIColumnSchema &ai_column,
                     const common::ObString &user_content,
                     bool &is_embedding,
                     common::ObString &result);
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_AI_EXECUTOR_H_
