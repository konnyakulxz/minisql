#include "record/row.h"

uint32_t Row::SerializeTo(char *buf, Schema *schema) const {//buf表示序列化的起始地址，返回值表示写入的字节数
  //序列化格式:|field_cnt|null_bitmap|field_data_1|...|field_data_N|，不用记录rowid
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");
  uint32_t offset=0;
  uint32_t field_cnt=static_cast<uint32_t>(fields_.size());
  MACH_WRITE_UINT32(buf+offset,field_cnt);
  offset+=sizeof(uint32_t);
  //null_bitmap:每一位对应一个field，1表示null（不写入数据区），0表示非null
  uint32_t null_bitmap_size=(field_cnt+7)/8;//上取整
  char *null_bitmap=buf+offset;
  memset(null_bitmap,0,null_bitmap_size);
  offset+=null_bitmap_size;
  for(uint32_t i=0;i<field_cnt;i++){
    if(fields_[i]->IsNull()){
      uint32_t byte_idx=i/8;
      uint8_t bit_idx=i%8;
      null_bitmap[byte_idx]|=(1<<bit_idx);
    }
    else offset+=fields_[i]->SerializeTo(buf+offset);
  }
  return offset;
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {//buf表示反序列化的起始地址，返回值表示读取的字节数
  ASSERT(schema!=nullptr, "Invalid schema before serialize.");
  ASSERT(fields_.empty(), "Non empty field in row.");
  uint32_t offset=0;
  uint32_t field_cnt=MACH_READ_UINT32(buf+offset);
  offset+=sizeof(uint32_t);
  uint32_t null_bitmap_size=(field_cnt+7)/8;
  char *null_bitmap=buf+offset;
  offset+=null_bitmap_size;
  for(uint32_t i=0;i<field_cnt;i++){
    uint32_t byte_idx=i/8;
    uint8_t bit_idx=i%8;
    bool is_null=(null_bitmap[byte_idx]>>bit_idx)&1;
    TypeId type=schema->GetColumn(i)->GetType();//获取第i列的类型
    Field *field=nullptr;
    offset+=Field::DeserializeFrom(buf+offset,type,&field,is_null);//如果是null则返回0
    fields_.push_back(field);
  }
  return offset;
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  ASSERT(schema!=nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount()==fields_.size(), "Fields size do not match schema's column size.");
  uint32_t size=sizeof(uint32_t);
  uint32_t field_cnt=static_cast<uint32_t>(fields_.size());
  size+=(field_cnt+7)/8;//null_bitmap的大小
  for(uint32_t i=0;i<field_cnt;i++) size+=fields_[i]->GetSerializedSize();
  return size;
}

void Row::GetKeyFromRow(const Schema *schema, const Schema *key_schema, Row &key_row) {
  auto columns = key_schema->GetColumns();
  std::vector<Field> fields;
  uint32_t idx;
  for (auto column : columns) {
    schema->GetColumnIndex(column->GetName(), idx);
    fields.emplace_back(*this->GetField(idx));
  }
  key_row = Row(fields);
}