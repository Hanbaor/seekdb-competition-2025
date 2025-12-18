/*
 * Copyright (c) 2025 OceanBase.
 * FTS 通用列值类型 - 支持多种数据类型的 tagged union
 */

#ifndef OB_FTS_COLUMN_VALUE_H_
#define OB_FTS_COLUMN_VALUE_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/oblog/ob_log_module.h"
#include "common/object/ob_obj_type.h"
#include "sql/engine/expr/ob_expr.h"

namespace oceanbase
{
namespace sql
{

/**
 * 通用列值类型 - 使用 tagged union 支持多种数据类型
 */
struct ObFtsColumnValue
{
  enum Type {
    TYPE_NULL = 0,
    TYPE_INT64,      // 整数类型 (包括 INT, BIGINT, TINYINT 等)
    TYPE_UINT64,     // 无符号整数
    TYPE_DOUBLE,     // 浮点数
    TYPE_STRING,     // 字符串类型 (VARCHAR, TEXT 等)
    TYPE_DATETIME,   // 日期时间类型
  };

  Type type_;

  union {
    int64_t int_val_;
    uint64_t uint_val_;
    double double_val_;
    struct {
      char *ptr_;
      int32_t len_;
    } str_val_;
    int64_t datetime_val_;  // 微秒时间戳
  };

  // 默认构造
  ObFtsColumnValue() : type_(TYPE_NULL), int_val_(0) {}

  // ========== 类型安全的构造函数 ==========
  static ObFtsColumnValue make_int(int64_t v)
  {
    ObFtsColumnValue val;
    val.type_ = TYPE_INT64;
    val.int_val_ = v;
    return val;
  }

  static ObFtsColumnValue make_uint(uint64_t v)
  {
    ObFtsColumnValue val;
    val.type_ = TYPE_UINT64;
    val.uint_val_ = v;
    return val;
  }

  static ObFtsColumnValue make_double(double v)
  {
    ObFtsColumnValue val;
    val.type_ = TYPE_DOUBLE;
    val.double_val_ = v;
    return val;
  }

  static ObFtsColumnValue make_string(const common::ObString &s, common::ObIAllocator &alloc)
  {
    ObFtsColumnValue val;
    val.type_ = TYPE_STRING;
    if (s.length() > 0 && OB_NOT_NULL(s.ptr())) {
      char *buf = static_cast<char*>(alloc.alloc(s.length()));
      if (OB_NOT_NULL(buf)) {
        MEMCPY(buf, s.ptr(), s.length());
        val.str_val_.ptr_ = buf;
        val.str_val_.len_ = s.length();
      } else {
        val.str_val_.ptr_ = nullptr;
        val.str_val_.len_ = 0;
      }
    } else {
      val.str_val_.ptr_ = nullptr;
      val.str_val_.len_ = 0;
    }
    return val;
  }

  static ObFtsColumnValue make_string_shallow(const char *ptr, int32_t len)
  {
    ObFtsColumnValue val;
    val.type_ = TYPE_STRING;
    val.str_val_.ptr_ = const_cast<char*>(ptr);
    val.str_val_.len_ = len;
    return val;
  }

  static ObFtsColumnValue make_datetime(int64_t v)
  {
    ObFtsColumnValue val;
    val.type_ = TYPE_DATETIME;
    val.datetime_val_ = v;
    return val;
  }

  static ObFtsColumnValue make_null()
  {
    ObFtsColumnValue val;
    val.type_ = TYPE_NULL;
    val.int_val_ = 0;
    return val;
  }

  // ========== 类型安全的获取函数 ==========
  int64_t get_int() const
  {
    return (type_ == TYPE_INT64) ? int_val_ : 0;
  }

  uint64_t get_uint() const
  {
    return (type_ == TYPE_UINT64) ? uint_val_ : 0;
  }

  double get_double() const
  {
    return (type_ == TYPE_DOUBLE) ? double_val_ : 0.0;
  }

  common::ObString get_string() const
  {
    if (type_ == TYPE_STRING) {
      return common::ObString(str_val_.len_, str_val_.ptr_);
    }
    return common::ObString();
  }

  int64_t get_datetime() const
  {
    return (type_ == TYPE_DATETIME) ? datetime_val_ : 0;
  }

  bool is_null() const
  {
    return type_ == TYPE_NULL;
  }

  // ========== 比较操作（用于过滤）==========
  int compare(const ObFtsColumnValue &other) const
  {
    if (type_ != other.type_) {
      // 类型不同，按类型值比较
      return static_cast<int>(type_) - static_cast<int>(other.type_);
    }

    switch (type_) {
      case TYPE_NULL:
        return 0;
      case TYPE_INT64:
        return (int_val_ < other.int_val_) ? -1 : ((int_val_ > other.int_val_) ? 1 : 0);
      case TYPE_UINT64:
        return (uint_val_ < other.uint_val_) ? -1 : ((uint_val_ > other.uint_val_) ? 1 : 0);
      case TYPE_DOUBLE:
        return (double_val_ < other.double_val_) ? -1 : ((double_val_ > other.double_val_) ? 1 : 0);
      case TYPE_STRING: {
        common::ObString s1(str_val_.len_, str_val_.ptr_);
        common::ObString s2(other.str_val_.len_, other.str_val_.ptr_);
        return s1.compare(s2);
      }
      case TYPE_DATETIME:
        return (datetime_val_ < other.datetime_val_) ? -1 : ((datetime_val_ > other.datetime_val_) ? 1 : 0);
      default:
        return 0;
    }
  }

  bool operator==(const ObFtsColumnValue &other) const
  {
    // 快速路径：类型不同直接返回 false
    if (type_ != other.type_) {
      return false;
    }
    
    // 快速比较，避免调用完整的 compare()
    switch (type_) {
      case TYPE_NULL:
        return true;
      case TYPE_INT64:
        return int_val_ == other.int_val_;
      case TYPE_UINT64:
        return uint_val_ == other.uint_val_;
      case TYPE_DOUBLE:
        return double_val_ == other.double_val_;
      case TYPE_STRING:
        // 先比较长度（快速失败路径）
        if (str_val_.len_ != other.str_val_.len_) {
          return false;
        }
        if (str_val_.len_ == 0) {
          return true;
        }
        // 使用 memcmp 直接比较，避免创建 ObString 对象
        return MEMCMP(str_val_.ptr_, other.str_val_.ptr_, str_val_.len_) == 0;
      case TYPE_DATETIME:
        return datetime_val_ == other.datetime_val_;
      default:
        return false;
    }
  }

  bool operator<(const ObFtsColumnValue &other) const
  {
    return compare(other) < 0;
  }

  bool operator<=(const ObFtsColumnValue &other) const
  {
    return compare(other) <= 0;
  }

  bool operator>(const ObFtsColumnValue &other) const
  {
    return compare(other) > 0;
  }

  bool operator>=(const ObFtsColumnValue &other) const
  {
    return compare(other) >= 0;
  }

  bool operator!=(const ObFtsColumnValue &other) const
  {
    return compare(other) != 0;
  }

  // ========== 从 ObDatum 转换 ==========
  static int from_datum(const ObDatum &datum,
                        common::ObObjType obj_type,
                        common::ObIAllocator &alloc,
                        ObFtsColumnValue &out)
  {
    int ret = common::OB_SUCCESS;

    if (datum.is_null()) {
      out = make_null();
      return ret;
    }

    switch (obj_type) {
      // 有符号整数类型
      case common::ObTinyIntType:
      case common::ObSmallIntType:
      case common::ObMediumIntType:
      case common::ObInt32Type:
      case common::ObIntType:
        out = make_int(datum.get_int());
        break;

      // 无符号整数类型
      case common::ObUTinyIntType:
      case common::ObUSmallIntType:
      case common::ObUMediumIntType:
      case common::ObUInt32Type:
      case common::ObUInt64Type:
        out = make_uint(datum.get_uint64());
        break;

      // 浮点类型
      case common::ObFloatType:
      case common::ObUFloatType:
        out = make_double(static_cast<double>(datum.get_float()));
        break;

      case common::ObDoubleType:
      case common::ObUDoubleType:
        out = make_double(datum.get_double());
        break;

      // 字符串类型
      case common::ObVarcharType:
      case common::ObCharType:
      case common::ObTinyTextType:
      case common::ObTextType:
      case common::ObMediumTextType:
      case common::ObLongTextType: {
        common::ObString str = datum.get_string();
        out = make_string(str, alloc);
        break;
      }

      // 日期时间类型
      case common::ObDateTimeType:
      case common::ObTimestampType:
        out = make_datetime(datum.get_datetime());
        break;

      case common::ObDateType:
        out = make_int(datum.get_date());
        break;

      case common::ObTimeType:
        out = make_int(datum.get_time());
        break;

      // 默认处理
      default:
        // 尝试作为整数处理
        if (ob_is_integer_type(obj_type)) {
          out = make_int(datum.get_int());
        } else if (ob_is_string_type(obj_type)) {
          common::ObString str = datum.get_string();
          out = make_string(str, alloc);
        } else {
          out = make_null();
          SQL_DAS_LOG(DEBUG, "FTS unsupported column type, treating as NULL",
                      K(obj_type));
        }
        break;
    }

    return ret;
  }

  // ========== 从 ObDatum 浅拷贝转换（字符串不分配内存）==========
  static int from_datum_shallow(const ObDatum &datum,
                                common::ObObjType obj_type,
                                ObFtsColumnValue &out)
  {
    int ret = common::OB_SUCCESS;
    if (datum.is_null()) {
      out = make_null();
      return ret;
    }
    switch (obj_type) {
      case common::ObTinyIntType:
      case common::ObSmallIntType:
      case common::ObMediumIntType:
      case common::ObInt32Type:
      case common::ObIntType:
        out = make_int(datum.get_int());
        break;
      case common::ObUTinyIntType:
      case common::ObUSmallIntType:
      case common::ObUMediumIntType:
      case common::ObUInt32Type:
      case common::ObUInt64Type:
        out = make_uint(datum.get_uint64());
        break;
      case common::ObFloatType:
      case common::ObUFloatType:
        out = make_double(static_cast<double>(datum.get_float()));
        break;
      case common::ObDoubleType:
      case common::ObUDoubleType:
        out = make_double(datum.get_double());
        break;
      case common::ObVarcharType:
      case common::ObCharType:
      case common::ObTinyTextType:
      case common::ObTextType:
      case common::ObMediumTextType:
      case common::ObLongTextType: {
        common::ObString str = datum.get_string();
        out = make_string_shallow(str.ptr(), str.length());  // 浅拷贝
        break;
      }
      case common::ObDateTimeType:
      case common::ObTimestampType:
        out = make_datetime(datum.get_datetime());
        break;
      case common::ObDateType:
        out = make_int(datum.get_date());
        break;
      case common::ObTimeType:
        out = make_int(datum.get_time());
        break;
      default:
        if (ob_is_integer_type(obj_type)) {
          out = make_int(datum.get_int());
        } else if (ob_is_string_type(obj_type)) {
          common::ObString str = datum.get_string();
          out = make_string_shallow(str.ptr(), str.length());
        } else {
          out = make_null();
        }
        break;
    }
    return ret;
  }

  // ========== 深拷贝 ==========
  int deep_copy(common::ObIAllocator &alloc, ObFtsColumnValue &out) const
  {
    int ret = common::OB_SUCCESS;

    out.type_ = type_;

    switch (type_) {
      case TYPE_NULL:
        out.int_val_ = 0;
        break;
      case TYPE_INT64:
        out.int_val_ = int_val_;
        break;
      case TYPE_UINT64:
        out.uint_val_ = uint_val_;
        break;
      case TYPE_DOUBLE:
        out.double_val_ = double_val_;
        break;
      case TYPE_STRING: {
        if (str_val_.len_ > 0 && OB_NOT_NULL(str_val_.ptr_)) {
          char *buf = static_cast<char*>(alloc.alloc(str_val_.len_));
          if (OB_ISNULL(buf)) {
            ret = common::OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, str_val_.ptr_, str_val_.len_);
            out.str_val_.ptr_ = buf;
            out.str_val_.len_ = str_val_.len_;
          }
        } else {
          out.str_val_.ptr_ = nullptr;
          out.str_val_.len_ = 0;
        }
        break;
      }
      case TYPE_DATETIME:
        out.datetime_val_ = datetime_val_;
        break;
      default:
        out.type_ = TYPE_NULL;
        out.int_val_ = 0;
        break;
    }

    return ret;
  }

  // 获取类型名称（用于调试）
  const char *get_type_name() const
  {
    switch (type_) {
      case TYPE_NULL: return "NULL";
      case TYPE_INT64: return "INT64";
      case TYPE_UINT64: return "UINT64";
      case TYPE_DOUBLE: return "DOUBLE";
      case TYPE_STRING: return "STRING";
      case TYPE_DATETIME: return "DATETIME";
      default: return "UNKNOWN";
    }
  }

  TO_STRING_KV("type", get_type_name(), K_(int_val));
};

} // namespace sql
} // namespace oceanbase

#endif // OB_FTS_COLUMN_VALUE_H_
