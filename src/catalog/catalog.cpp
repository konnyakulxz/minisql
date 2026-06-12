#include "catalog/catalog.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  // check valid
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  // get table and index nums
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  // create metadata and read value
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}

uint32_t CatalogMeta::GetSerializedSize() const {
  return 3*sizeof(uint32_t)+table_meta_pages_.size()*2*sizeof(uint32_t)+index_meta_pages_.size()*2*sizeof(uint32_t);
}

CatalogMeta::CatalogMeta() {}

CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager), lock_manager_(lock_manager), log_manager_(log_manager) {
  if(init){//首次创建数据库，新建空的CatalogMeta
    catalog_meta_=CatalogMeta::NewInstance();
    next_table_id_=0;
    next_index_id_=0;
    FlushCatalogMetaPage();
  }
  else{//重新打开数据库，从磁盘加载CatalogMeta
    auto meta_page=buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
    auto *meta_data=meta_page->GetData();
    catalog_meta_=CatalogMeta::DeserializeFrom(meta_data);
    next_table_id_=catalog_meta_->GetNextTableId();
    next_index_id_=catalog_meta_->GetNextIndexId();
    //加载所有表
    for(auto &iter:catalog_meta_->table_meta_pages_){
      LoadTable(iter.first,iter.second);
    }
    //加载所有索引
    for(auto &iter:catalog_meta_->index_meta_pages_){
      LoadIndex(iter.first,iter.second);
    }
    buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID,false);
  }
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto iter : tables_) {
    delete iter.second;
  }
  for (auto iter : indexes_) {
    delete iter.second;
  }
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn, TableInfo *&table_info) {
  if(table_names_.find(table_name)!=table_names_.end()) return DB_TABLE_ALREADY_EXIST;
  auto *deep_schema=Schema::DeepCopySchema(schema);
  auto table_heap=TableHeap::Create(buffer_pool_manager_,deep_schema,txn,log_manager_,lock_manager_);
  table_id_t table_id=next_table_id_++;
  auto *table_meta=TableMetadata::Create(table_id,table_name,table_heap->GetFirstPageId(),deep_schema);
  page_id_t meta_page_id;
  auto meta_page=buffer_pool_manager_->NewPage(meta_page_id);
  if(meta_page==nullptr) return DB_FAILED;
  table_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id,true);
  catalog_meta_->table_meta_pages_.emplace(table_id,meta_page_id);
  table_info=TableInfo::Create();
  table_info->Init(table_meta,table_heap);
  table_names_.emplace(table_name,table_id);
  tables_.emplace(table_id,table_info);
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
  auto it=table_names_.find(table_name);
  if(it==table_names_.end()) return DB_TABLE_NOT_EXIST;
  return GetTable(it->second,table_info);
}

dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
  for(auto &iter:tables_) tables.push_back(iter.second);
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn, IndexInfo *&index_info,
                                    const string &index_type) {
  auto table_it=table_names_.find(table_name);
  if(table_it==table_names_.end()) return DB_TABLE_NOT_EXIST;
  table_id_t table_id=table_it->second;
  TableInfo *table_info=tables_[table_id];
  auto &index_map=index_names_[table_name];
  if(index_map.find(index_name)!=index_map.end()) return DB_INDEX_ALREADY_EXIST;
  vector<uint32_t> key_map;
  for(auto &col_name:index_keys){
    uint32_t column_index;
    if(table_info->GetSchema()->GetColumnIndex(col_name,column_index)!=DB_SUCCESS) return DB_COLUMN_NAME_NOT_EXIST;
    key_map.push_back(column_index);
  }
  index_id_t index_id=next_index_id_++;
  auto *index_meta=IndexMetadata::Create(index_id,index_name,table_id,key_map);
  page_id_t meta_page_id;
  auto meta_page=buffer_pool_manager_->NewPage(meta_page_id);
  if(meta_page==nullptr) return DB_FAILED;
  index_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id,true);
  catalog_meta_->index_meta_pages_.emplace(index_id,meta_page_id);
  index_info=IndexInfo::Create();
  index_info->Init(index_meta,table_info,buffer_pool_manager_);
  index_map.emplace(index_name,index_id);
  indexes_.emplace(index_id,index_info);
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  auto table_it=index_names_.find(table_name);
  if(table_it==index_names_.end()) return DB_TABLE_NOT_EXIST;
  auto index_it=table_it->second.find(index_name);
  if(index_it==table_it->second.end()) return DB_INDEX_NOT_FOUND;
  auto info_it=indexes_.find(index_it->second);
  if(info_it==indexes_.end()) return DB_INDEX_NOT_FOUND;
  index_info=info_it->second;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTableIndexes(const std::string &table_name, std::vector<IndexInfo *> &indexes) const {
  auto table_it=index_names_.find(table_name);
  if(table_it==index_names_.end()) return DB_TABLE_NOT_EXIST;
  for(auto &iter:table_it->second) indexes.push_back(indexes_.at(iter.second));
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropTable(const string &table_name) {
  auto it=table_names_.find(table_name);
  if(it==table_names_.end()) return DB_TABLE_NOT_EXIST;
  table_id_t table_id=it->second;
  //先删该表的所有索引
  if(index_names_.find(table_name)!=index_names_.end()){
    auto index_map=index_names_[table_name];
    for(auto &idx_iter:index_map){
      indexes_[idx_iter.second]->GetIndex()->Destroy();
      catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_,idx_iter.second);
      delete indexes_[idx_iter.second];
      indexes_.erase(idx_iter.second);
    }
    index_names_.erase(table_name);
  }
  //删TableHeap
  tables_[table_id]->GetTableHeap()->DeleteTable();
  //删表元数据页
  page_id_t meta_page_id=catalog_meta_->table_meta_pages_[table_id];
  buffer_pool_manager_->DeletePage(meta_page_id);
  catalog_meta_->table_meta_pages_.erase(table_id);
  delete tables_[table_id];
  tables_.erase(table_id);
  table_names_.erase(table_name);
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  auto table_it=index_names_.find(table_name);
  if(table_it==index_names_.end()) return DB_TABLE_NOT_EXIST;
  auto idx_it=table_it->second.find(index_name);
  if(idx_it==table_it->second.end()) return DB_INDEX_NOT_FOUND;
  index_id_t index_id=idx_it->second;
  indexes_[index_id]->GetIndex()->Destroy();
  catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_,index_id);
  delete indexes_[index_id];
  indexes_.erase(index_id);
  table_it->second.erase(index_name);
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::FlushCatalogMetaPage() const {
  auto meta_page=buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  if(meta_page==nullptr) return DB_FAILED;
  catalog_meta_->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID,true);
  return DB_SUCCESS;
}

dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id){
  auto page=buffer_pool_manager_->FetchPage(page_id);
  if(page==nullptr) return DB_FAILED;
  TableMetadata *table_meta=nullptr;
  TableMetadata::DeserializeFrom(page->GetData(),table_meta);
  buffer_pool_manager_->UnpinPage(page_id,false);
  //根据元数据创建TableHeap和TableInfo
  auto table_heap=TableHeap::Create(buffer_pool_manager_,table_meta->GetFirstPageId(),table_meta->GetSchema(),
                                    log_manager_,lock_manager_);
  auto *table_info=TableInfo::Create();
  table_info->Init(table_meta,table_heap);
  table_names_.emplace(table_meta->GetTableName(),table_id);
  tables_.emplace(table_id,table_info);
  return DB_SUCCESS;
}

dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id){
  auto page=buffer_pool_manager_->FetchPage(page_id);
  if(page==nullptr) return DB_FAILED;
  IndexMetadata *index_meta=nullptr;
  IndexMetadata::DeserializeFrom(page->GetData(),index_meta);
  buffer_pool_manager_->UnpinPage(page_id,false);
  //根据元数据找到对应的表，创建IndexInfo
  TableInfo *table_info=nullptr;
  dberr_t ret=GetTable(index_meta->GetTableId(),table_info);
  if(ret!=DB_SUCCESS) return ret;
  auto *index_info=IndexInfo::Create();
  index_info->Init(index_meta,table_info,buffer_pool_manager_);
  //维护索引名映射: table_name -> index_name -> index_id
  if(index_names_.find(table_info->GetTableName())==index_names_.end()){
    index_names_.emplace(table_info->GetTableName(),std::unordered_map<std::string,index_id_t>());
  }
  index_names_[table_info->GetTableName()].emplace(index_meta->GetIndexName(),index_id);
  indexes_.emplace(index_id,index_info);
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  auto it=tables_.find(table_id);
  if(it==tables_.end()) return DB_TABLE_NOT_EXIST;
  table_info=it->second;
  return DB_SUCCESS;
}