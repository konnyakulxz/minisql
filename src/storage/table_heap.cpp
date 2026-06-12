#include "storage/table_heap.h"

TableHeap::TableHeap(BufferPoolManager *buffer_pool_manager, Schema *schema, Txn *txn, LogManager *log_manager,
                     LockManager *lock_manager)
    : buffer_pool_manager_(buffer_pool_manager),
      schema_(schema),
      log_manager_(log_manager),
      lock_manager_(lock_manager) {
  page_id_t page_id;
  auto page=reinterpret_cast<TablePage *>(buffer_pool_manager_->NewPage(page_id));
  page->Init(page_id,INVALID_PAGE_ID,log_manager_,txn);//第一页的prev_page_id为INVALID_PAGE_ID
  first_page_id_=page_id;
  free_page_list_.push_back(page_id);//新页加入空闲链表
  buffer_pool_manager_->UnpinPage(page_id,true);//整个transaction完成，可以取消固定了
}

bool TableHeap::InsertTuple(Row &row, Txn *txn) {
  //在空闲链表中寻找有足够空间的页来插入
  auto it=free_page_list_.begin();
  while(it!=free_page_list_.end()){
    page_id_t page_id=*it;
    auto page=reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));
    if(page==nullptr){
      it=free_page_list_.erase(it);
      continue;
    }
    if(page->InsertTuple(row,schema_,txn,lock_manager_,log_manager_)){//插入成功
      buffer_pool_manager_->UnpinPage(page_id,true);//被修改了，dirty=true，transaction结束要unpin
      return true;
    }
    //当前页剩余空间不够放这条记录
    if(page->GetFreeSpaceRemaining()<TablePage::SIZE_TUPLE) it=free_page_list_.erase(it);//连最小的tuple都装不下，真正满了，从空闲链表中移除
    else ++it;
    buffer_pool_manager_->UnpinPage(page_id,false);
  }
  //所有有空闲的页都不够放，新建一页
  page_id_t new_page_id;
  auto new_page=reinterpret_cast<TablePage *>(buffer_pool_manager_->NewPage(new_page_id));
  new_page->Init(new_page_id,INVALID_PAGE_ID,log_manager_,txn);
  new_page->InsertTuple(row,schema_,txn,lock_manager_,log_manager_);
  new_page->SetNextPageId(first_page_id_);
  if(first_page_id_!=INVALID_PAGE_ID){
    auto old_first=reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(first_page_id_));
    old_first->SetPrevPageId(new_page_id);
    buffer_pool_manager_->UnpinPage(first_page_id_,true);
  }
  first_page_id_=new_page_id;
  free_page_list_.push_back(new_page_id);
  buffer_pool_manager_->UnpinPage(new_page_id,true);
  return true;
}

bool TableHeap::MarkDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  // If the page could not be found, then abort the recovery.
  if (page == nullptr) {
    return false;
  }
  // Otherwise, mark the tuple as deleted.
  page->WLatch();
  page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  return true;
}

bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  auto page=reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if(page==nullptr) return false;
  Row old_row(rid);
  if(page->UpdateTuple(row,&old_row,schema_,txn,lock_manager_,log_manager_)){
    row.SetRowId(rid);
    buffer_pool_manager_->UnpinPage(rid.GetPageId(),true);
    return true;
  }
  //空间不足，把旧的tuple删了，插入新的tuple
  page->MarkDelete(rid,txn,lock_manager_,log_manager_);
  page->ApplyDelete(rid,txn,log_manager_);
  buffer_pool_manager_->UnpinPage(rid.GetPageId(),true);
  return InsertTuple(row,txn);
}

void TableHeap::ApplyDelete(const RowId &rid, Txn *txn) {
  auto page=reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if(page==nullptr) return;
  page->WLatch();
  page->ApplyDelete(rid,txn,log_manager_);
  page->WUnlatch();
  //删除后页内有新空间，加入空闲链表
  bool in_list=false;
  for(auto id:free_page_list_) if(id==rid.GetPageId()){in_list=true;break;}
  if(!in_list) free_page_list_.push_back(rid.GetPageId());
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(),true);
}

void TableHeap::RollbackDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  // Rollback to delete.
  page->WLatch();
  page->RollbackDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

bool TableHeap::GetTuple(Row *row, Txn *txn) {
  auto page=reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId()));
  if(page==nullptr) return false;
  bool res=page->GetTuple(row,schema_,txn,lock_manager_);
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(),false);
  return res;
}

void TableHeap::DeleteTable(page_id_t page_id) {
  if (page_id != INVALID_PAGE_ID) {
    auto temp_table_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));  // 删除table_heap
    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID)
      DeleteTable(temp_table_page->GetNextPageId());
    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  } else {
    DeleteTable(first_page_id_);
  }
}

TableIterator TableHeap::Begin(Txn *txn) {
  page_id_t page_id=first_page_id_;
  while(page_id!=INVALID_PAGE_ID){
    auto page=reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));
    RowId first_rid;
    if(page->GetFirstTupleRid(&first_rid)){//当前page内有tuple，则该tuple即为first tuple
      buffer_pool_manager_->UnpinPage(page_id,false);
      return TableIterator(this,first_rid,txn);
    }
    page_id_t next_page_id=page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(page_id,false);
    page_id=next_page_id;
  }
  return End();
}

TableIterator TableHeap::End() {
  return TableIterator(this,INVALID_ROWID,nullptr);
}