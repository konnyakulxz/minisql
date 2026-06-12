#include "record/schema.h"

uint32_t Schema::SerializeTo(char *buf) const {//buf表示序列化的起始地址，返回值表示写入的字节数
  //序列化格式:|MAGIC_NUM|col_cnt|is_manage_|Column1|Column2|...|
  uint32_t offset=0;
  MACH_WRITE_UINT32(buf+offset,SCHEMA_MAGIC_NUM);
  offset+=sizeof(uint32_t);
  uint32_t col_cnt=static_cast<uint32_t>(columns_.size());
  MACH_WRITE_UINT32(buf+offset,col_cnt);
  offset+=sizeof(uint32_t);
  MACH_WRITE_TO(bool,buf+offset,is_manage_);
  offset+=sizeof(bool);
  for(auto col:columns_) offset+=col->SerializeTo(buf+offset);//依次序列化每个列
  return offset;
}

uint32_t Schema::GetSerializedSize() const {
  uint32_t size=sizeof(uint32_t)+sizeof(uint32_t)+sizeof(bool);
  for(auto col:columns_) size+=col->GetSerializedSize();
  return size;
}

uint32_t Schema::DeserializeFrom(char *buf, Schema *&schema) {//buf表示反序列化的起始地址，schema表示反序列化得到的Schema对象指针，返回值表示读取的字节数
  uint32_t offset=0;
  uint32_t magic=MACH_READ_UINT32(buf+offset);
  ASSERT(magic==SCHEMA_MAGIC_NUM,"Schema magic number mismatch.");//魔数不匹配说明反序列化失败
  offset+=sizeof(uint32_t);
  uint32_t col_cnt=MACH_READ_UINT32(buf+offset);
  offset+=sizeof(uint32_t);
  bool is_manage=MACH_READ_FROM(bool,buf+offset);
  offset+=sizeof(bool);
  std::vector<Column *> cols;
  for(uint32_t i=0;i<col_cnt;i++){
    Column *col=nullptr;
    offset+=Column::DeserializeFrom(buf+offset,col);
    cols.push_back(col);
  }
  schema=new Schema(cols,is_manage);
  return offset;
}