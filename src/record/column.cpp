#include "record/column.h"

#include "glog/logging.h"

Column::Column(std::string column_name, TypeId type, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)), type_(type), table_ind_(index), nullable_(nullable), unique_(unique) {
  ASSERT(type != TypeId::kTypeChar, "Wrong constructor for CHAR type.");
  switch (type) {
    case TypeId::kTypeInt:
      len_ = sizeof(int32_t);
      break;
    case TypeId::kTypeFloat:
      len_ = sizeof(float_t);
      break;
    default:
      ASSERT(false, "Unsupported column type.");
  }
}

Column::Column(std::string column_name, TypeId type, uint32_t length, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)),
      type_(type),
      len_(length),
      table_ind_(index),
      nullable_(nullable),
      unique_(unique) {
  ASSERT(type == TypeId::kTypeChar, "Wrong constructor for non-VARCHAR type.");
}

Column::Column(const Column *other)
    : name_(other->name_),
      type_(other->type_),
      len_(other->len_),
      table_ind_(other->table_ind_),
      nullable_(other->nullable_),
      unique_(other->unique_) {}

uint32_t Column::SerializeTo(char *buf) const {//buf表示序列化的起始地址，返回值表示写入的字节数
  //序列化格式:|MAGIC_NUM|name_len|name|type|len|table_ind|nullable|unique|
  uint32_t offset=0;
  MACH_WRITE_UINT32(buf+offset,COLUMN_MAGIC_NUM);
  offset+=sizeof(uint32_t);
  MACH_WRITE_UINT32(buf+offset,name_.length());
  offset+=sizeof(uint32_t);
  MACH_WRITE_STRING(buf+offset,name_);
  offset+=name_.length();
  MACH_WRITE_TO(TypeId,buf+offset,type_);
  offset+=sizeof(TypeId);
  MACH_WRITE_UINT32(buf+offset,len_);
  offset+=sizeof(uint32_t);
  MACH_WRITE_UINT32(buf+offset,table_ind_);
  offset+=sizeof(uint32_t);
  MACH_WRITE_TO(bool,buf+offset,nullable_);
  offset+=sizeof(bool);
  MACH_WRITE_TO(bool,buf+offset,unique_);
  offset+=sizeof(bool);
  return offset;
}

uint32_t Column::GetSerializedSize() const {
  return 2*sizeof(uint32_t)+name_.length()+sizeof(TypeId)+2*sizeof(uint32_t)+2*sizeof(bool);
}

uint32_t Column::DeserializeFrom(char *buf, Column *&column) {//buf表示反序列化的起始地址，column表示反序列化得到的Column对象指针，返回值表示读取的字节数
  uint32_t offset=0;
  uint32_t magic=MACH_READ_UINT32(buf+offset);
  ASSERT(magic==COLUMN_MAGIC_NUM,"Column magic number mismatch.");//魔数不匹配说明反序列化失败
  offset+=sizeof(uint32_t);
  uint32_t name_len=MACH_READ_UINT32(buf+offset);
  offset+=sizeof(uint32_t);
  std::string column_name(buf+offset,name_len);
  offset+=name_len;
  TypeId type=MACH_READ_FROM(TypeId,buf+offset);
  offset+=sizeof(TypeId);
  uint32_t len=MACH_READ_UINT32(buf+offset);
  offset+=sizeof(uint32_t);
  uint32_t table_ind=MACH_READ_UINT32(buf+offset);
  offset+=sizeof(uint32_t);
  bool nullable=MACH_READ_FROM(bool,buf+offset);
  offset+=sizeof(bool);
  bool unique=MACH_READ_FROM(bool,buf+offset);
  offset+=sizeof(bool);
  if(type==TypeId::kTypeChar) column=new Column(column_name,type,len,table_ind,nullable,unique);
  else column=new Column(column_name,type,table_ind,nullable,unique);
  return offset;
}