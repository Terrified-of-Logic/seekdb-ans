/*
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

#define USING_LOG_PREFIX SQL_RESV
#include <string.h>
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/cmd/ob_load_data_parser.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

namespace
{

// ---------------------------------------------------------------------------
// file sql contract helpers: DESCRIBE SELECT and COPY ... TO.
// Both are parsed as T_EXPLAIN statements by the grammar and carry a marker
// node in children_[0] (T_INT: 1 = DESCRIBE SELECT, 2 = COPY ... TO).
// ---------------------------------------------------------------------------

int explain_file_name_equal(const ParseNode *name_node, const char *name)
{
  int equal = 0;
  if (OB_NOT_NULL(name_node) && NULL != name_node->str_value_ && NULL != name) {
    const int64_t name_len = static_cast<int64_t>(strlen(name));
    if (name_node->str_len_ == name_len) {
      equal = 1;
      for (int64_t i = 0; equal && i < name_len; ++i) {
        char left = name_node->str_value_[i];
        char right = name[i];
        if (left >= 'A' && left <= 'Z') {
          left = static_cast<char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
          right = static_cast<char>(right - 'A' + 'a');
        }
        if (left != right) {
          equal = 0;
        }
      }
    }
  }
  return equal;
}

int resolve_describe_column(const ObSelectStmt &select_stmt,
                            const SelectItem &select_item,
                            ObString &column_name,
                            ObString &type_name,
                            bool &nullable)
{
  int ret = OB_SUCCESS;
  const ObRawExpr *expr = select_item.expr_;
  bool found = false;
  nullable = true;
  column_name = select_item.alias_name_;
  type_name = ObString::make_string("VARCHAR");
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("describe select item has no expression", K(ret));
  }
  if (OB_SUCC(ret) && column_name.empty() && T_REF_COLUMN == expr->get_expr_type()) {
    column_name = static_cast<const ObColumnRefRawExpr *>(expr)->get_column_name();
  }
  if (OB_SUCC(ret) && column_name.empty()) {
    column_name = select_item.expr_name_;
  }
  if (OB_SUCC(ret) && column_name.empty()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("describe select item has no name", K(ret));
  }
  // for file table columns the contract prints the inferred file type name
  if (OB_SUCC(ret) && T_REF_COLUMN == expr->get_expr_type()) {
    const ObColumnRefRawExpr *column_ref = static_cast<const ObColumnRefRawExpr *>(expr);
    const uint64_t table_id = column_ref->get_table_id();
    for (int64_t i = 0; OB_SUCC(ret) && !found && i < select_stmt.get_table_size(); ++i) {
      const TableItem *table_item = select_stmt.get_table_item(i);
      if (OB_ISNULL(table_item) || !table_item->is_file_table()
          || OB_ISNULL(table_item->file_table_def_)) {
        continue;
      } else if (table_item->table_id_ != table_id) {
        continue;
      } else {
        const ObFileTableDef *file_def = table_item->file_table_def_;
        for (int64_t j = 0; OB_SUCC(ret) && !found && j < file_def->columns_.count(); ++j) {
          const ObFileTableColumnDef &file_column = file_def->columns_.at(j);
          if (ObCharset::case_insensitive_equal(file_column.column_name_, column_name)) {
            type_name = ObString::make_string(
                ObFileScanUtils::column_type_name(file_column.type_));
            nullable = file_column.nullable_;
            found = true;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && !found) {
    const ObObjType obj_type = expr->get_result_type().get_type();
    switch (obj_type) {
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType:
      case ObUTinyIntType:
      case ObUSmallIntType:
      case ObUMediumIntType:
      case ObUInt32Type:
      case ObUInt64Type:
        type_name = ObString::make_string("BIGINT");
        break;
      case ObFloatType:
      case ObUFloatType:
      case ObDoubleType:
      case ObUDoubleType:
        type_name = ObString::make_string("DOUBLE");
        break;
      case ObNumberType:
      case ObUNumberType:
      case ObDecimalIntType:
        type_name = ObString::make_string("DECIMAL");
        break;
      case ObDateType:
        type_name = ObString::make_string("DATE");
        break;
      case ObDateTimeType:
        type_name = ObString::make_string("DATETIME");
        break;
      case ObTimestampType:
        type_name = ObString::make_string("TIMESTAMP");
        break;
      default:
        type_name = ObString::make_string("VARCHAR");
        break;
    }
    nullable = !expr->get_result_type().has_result_flag(NOT_NULL_FLAG);
  }
  return ret;
}

int build_describe_meta_sql(const ObSelectStmt &select_stmt, ObSqlString &meta_sql)
{
  int ret = OB_SUCCESS;
  const int64_t item_count = select_stmt.get_select_item_size();
  if (0 >= item_count) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("describe select has no select item", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < item_count; ++i) {
    ObString column_name;
    ObString type_name;
    bool nullable = true;
    if (OB_FAIL(resolve_describe_column(select_stmt,
                                        select_stmt.get_select_item(i),
                                        column_name,
                                        type_name,
                                        nullable))) {
      LOG_WARN("failed to describe select item", K(ret), K(i));
    } else if (0 == i) {
      if (OB_FAIL(meta_sql.append_fmt(
                    "select '%.*s' as column_name, '%.*s' as column_type, '%s' as `null`, "
                    "NULL as `key`, NULL as `default`, NULL as `extra`, %ld as ord",
                    column_name.length(), column_name.ptr(),
                    type_name.length(), type_name.ptr(),
                    nullable ? "YES" : "NO", i))) {
        LOG_WARN("failed to build describe meta sql", K(ret));
      }
    } else if (OB_FAIL(meta_sql.append_fmt(
                 " union all select '%.*s', '%.*s', '%s', NULL, NULL, NULL, %ld",
                 column_name.length(), column_name.ptr(),
                 type_name.length(), type_name.ptr(),
                 nullable ? "YES" : "NO", i))) {
      LOG_WARN("failed to build describe meta sql", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    ObSqlString wrapped_sql;
    if (OB_FAIL(wrapped_sql.append_fmt(
                  "select column_name, column_type, `null`, `key`, `default`, `extra` "
                  "from (%s) describe_file_sql order by ord", meta_sql.ptr()))) {
      LOG_WARN("failed to wrap describe meta sql", K(ret));
    } else if (OB_FAIL(meta_sql.assign(wrapped_sql))) {
      LOG_WARN("failed to assign describe meta sql", K(ret));
    }
  }
  return ret;
}

int resolve_describe_select(ObResolverParams &params,
                            ObIAllocator &allocator,
                            ObSQLSessionInfo &session,
                            const ParseNode &parse_tree,
                            ObStmt *&out_stmt)
{
  int ret = OB_SUCCESS;
  ParseNode *select_node = NULL;
  ObStmt *inner_stmt = NULL;
  ObSelectStmt *select_stmt = NULL;
  ObSqlString meta_sql;
  out_stmt = NULL;
  if (OB_UNLIKELY(T_EXPLAIN != parse_tree.type_ || 5 != parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid describe select statement", K(ret), K(parse_tree.type_));
  } else if (OB_ISNULL(select_node = parse_tree.children_[4])) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObResolver resolver(params);
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *select_node, inner_stmt))) {
      LOG_WARN("failed to resolve the statement of describe select", K(ret));
    } else if (OB_ISNULL(inner_stmt) || !inner_stmt->is_select_stmt()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "DESCRIBE on a non-select statement");
    } else if (FALSE_IT(select_stmt = static_cast<ObSelectStmt *>(inner_stmt))) {
    } else if (OB_FAIL(build_describe_meta_sql(*select_stmt, meta_sql))) {
      LOG_WARN("failed to build describe select meta sql", K(ret));
    } else {
      ParseResult parse_result;
      ObParser parser(allocator, session.get_sql_mode(), session.get_charsets4parser());
      ObString meta_sql_str = meta_sql.string();
      if (OB_FAIL(parser.parse(meta_sql_str, parse_result))) {
        LOG_WARN("failed to parse describe select meta sql", K(ret), K(meta_sql_str));
      } else if (OB_ISNULL(parse_result.result_tree_)
                 || 0 >= parse_result.result_tree_->num_child_
                 || OB_ISNULL(parse_result.result_tree_->children_[0])) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObStmt *meta_stmt = NULL;
        ObResolver meta_resolver(params);
        if (OB_FAIL(meta_resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                          *parse_result.result_tree_->children_[0],
                                          meta_stmt))) {
          LOG_WARN("failed to resolve describe select meta sql", K(ret));
        } else if (OB_ISNULL(meta_stmt)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          out_stmt = meta_stmt;
        }
      }
    }
  }
  return ret;
}

int resolve_copy_string_literal(const ParseNode &node,
                                const ParamStore *param_list,
                                ObString &value)
{
  int ret = OB_SUCCESS;
  value.reset();
  if (T_CHAR == node.type_ || T_VARCHAR == node.type_) {
    value.assign_ptr(node.str_value_, static_cast<int32_t>(node.str_len_));
  } else if (T_QUESTIONMARK == node.type_) {
    if (OB_ISNULL(param_list) || node.value_ < 0 || node.value_ >= param_list->count()
        || !param_list->at(node.value_).is_string_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid copy to path parameter", K(ret), K(node.value_));
    } else {
      value = param_list->at(node.value_).get_string();
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("copy to path must be a string literal", K(ret), K(node.type_));
  }
  return ret;
}

int parse_copy_options(const ParseNode *option_node,
                       bool &write_header,
                       ObString &nullstr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(option_node)) {
    // no options
  } else if (T_LINK_NODE != option_node->type_ || 2 != option_node->num_child_
             || OB_ISNULL(option_node->children_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid copy option node", K(ret));
  } else if (option_node->value_ >= 1 && option_node->value_ <= 3) {
    const ParseNode *name_node = option_node->children_[0];
    const ParseNode *value_node = option_node->children_[1];
    if (2 == option_node->value_) { // bare option, e.g. header
      if (explain_file_name_equal(value_node, "header")) {
        write_header = true;
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "this COPY option");
      }
    } else if (1 == option_node->value_) { // name name, e.g. format csv
      if (explain_file_name_equal(name_node, "format")
          && explain_file_name_equal(value_node, "csv")) {
        // csv is the only supported format
      } else if (explain_file_name_equal(name_node, "format")) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY format other than csv");
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "this COPY option");
      }
    } else { // name literal, e.g. nullstr 'NULL'
      if (explain_file_name_equal(name_node, "nullstr")) {
        if (OB_ISNULL(value_node)
            || (T_CHAR != value_node->type_ && T_VARCHAR != value_node->type_)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          nullstr.assign_ptr(value_node->str_value_,
                             static_cast<int32_t>(value_node->str_len_));
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "this COPY option");
      }
    }
  } else if (OB_FAIL(parse_copy_options(option_node->children_[0], write_header, nullstr))) {
    LOG_WARN("failed to parse copy option", K(ret));
  } else if (OB_FAIL(parse_copy_options(option_node->children_[1], write_header, nullstr))) {
    LOG_WARN("failed to parse copy option", K(ret));
  }
  return ret;
}

int resolve_copy_to(ObResolverParams &params,
                    ObIAllocator &allocator,
                    ObSQLSessionInfo &session,
                    const ParseNode &parse_tree,
                    ObStmt *&out_stmt)
{
  int ret = OB_SUCCESS;
  ParseNode *path_node = NULL;
  ParseNode *select_node = NULL;
  ObString path;
  ObString nullstr;
  bool write_header = false;
  out_stmt = NULL;
  if (OB_UNLIKELY(T_EXPLAIN != parse_tree.type_ || 6 != parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid copy to statement", K(ret), K(parse_tree.type_));
  } else if (OB_ISNULL(path_node = parse_tree.children_[2])
             || OB_ISNULL(select_node = parse_tree.children_[4])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(resolve_copy_string_literal(*path_node, params.param_list_, path))) {
    LOG_WARN("invalid copy to path", K(ret));
  } else if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("copy to path is empty", K(ret));
  } else if (OB_FAIL(parse_copy_options(parse_tree.children_[5], write_header, nullstr))) {
    LOG_WARN("invalid copy to option", K(ret));
  } else {
    ObStmt *inner_stmt = NULL;
    ObResolver resolver(params);
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *select_node, inner_stmt))) {
      LOG_WARN("failed to resolve the query of copy to", K(ret));
    } else if (OB_ISNULL(inner_stmt) || !inner_stmt->is_select_stmt()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY of a non-select statement");
    } else {
      ObSelectStmt *select_stmt = static_cast<ObSelectStmt *>(inner_stmt);
      ObSelectIntoItem *into_item = static_cast<ObSelectIntoItem *>(
          allocator.alloc(sizeof(ObSelectIntoItem)));
      ObExternalFileFormat external_format;
      if (OB_ISNULL(into_item)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate copy to into item", K(ret));
      } else {
        new (into_item) ObSelectIntoItem();
        into_item->into_type_ = T_INTO_OUTFILE;
        if (T_QUESTIONMARK == path_node->type_) {
          // fast parameterized SQL: resolve the path from the parameter store at run time
          into_item->outfile_name_.set_unknown(path_node->value_);
        } else {
          into_item->outfile_name_.set_varchar(path);
          into_item->outfile_name_.set_collation_type(
              session.get_local_collation_connection());
        }
        into_item->is_single_ = true;
        into_item->write_header_ = write_header;
        external_format.format_type_ = ObExternalFileFormat::CSV_FORMAT;
        external_format.csv_format_.cs_type_ =
            ObCharset::charset_type_by_coll(ObCharset::get_system_collation());
        external_format.csv_format_.field_term_str_ = ObString::make_string(",");
        external_format.csv_format_.line_term_str_ = ObString::make_string("\n");
        external_format.csv_format_.field_escaped_char_ = static_cast<int64_t>('\\');
        external_format.csv_format_.field_enclosed_char_ = INT64_MAX;
        external_format.csv_format_.file_extension_ = ObString::make_string(".csv");
        external_format.csv_format_.file_column_nums_ = 0;
        if (OB_FAIL(external_format.to_string_with_alloc(into_item->external_properties_,
                                                         allocator,
                                                         true))) {
          LOG_WARN("failed to serialize copy to csv format", K(ret));
        } else {
          select_stmt->set_select_into(into_item);
          out_stmt = select_stmt;
        }
      }
    }
  }
  return ret;
}

} // namespace

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (T_EXPLAIN == parse_tree.type_ && OB_NOT_NULL(parse_tree.children_)
      && OB_NOT_NULL(parse_tree.children_[0])
      && T_INT == parse_tree.children_[0]->type_
      && (1 == parse_tree.children_[0]->value_ || 2 == parse_tree.children_[0]->value_)) {
    // file sql contract: DESCRIBE SELECT (1) / COPY ... TO (2) markers
    ObStmt *file_sql_stmt = NULL;
    if (1 == parse_tree.children_[0]->value_) {
      ret = resolve_describe_select(params_, *allocator_, *session_info_, parse_tree, file_sql_stmt);
    } else {
      ret = resolve_copy_to(params_, *allocator_, *session_info_, parse_tree, file_sql_stmt);
    }
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to resolve file sql contract statement", K(ret));
    } else if (OB_ISNULL(file_sql_stmt)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      set_basic_stmt(file_sql_stmt);
    }
    return ret;
  }
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
