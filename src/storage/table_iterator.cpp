#include "storage/table_iterator.h"

#include "common/macros.h"
#include "storage/table_heap.h"

TableIterator::TableIterator(TableHeap *table_heap, RowId rid, Txn *txn)
    : table_heap_(table_heap), row_(rid), txn_(txn) {
  if(rid.GetPageId()!=INVALID_PAGE_ID){
    table_heap_->GetTuple(&row_, txn_);
  }
}

TableIterator::TableIterator(const TableIterator &other)
    : table_heap_(other.table_heap_), row_(other.row_), txn_(other.txn_) {}

TableIterator::~TableIterator() {}

bool TableIterator::operator==(const TableIterator &itr) const {
  return row_.GetRowId()==itr.row_.GetRowId();
}

bool TableIterator::operator!=(const TableIterator &itr) const {
  return !(*this==itr);
}

const Row &TableIterator::operator*() {
  return row_;
}

Row *TableIterator::operator->() {
  return &row_;
}

TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  table_heap_=itr.table_heap_;
  row_=itr.row_;
  txn_=itr.txn_;
  return *this;
}

// ++iter
TableIterator &TableIterator::operator++() {
  page_id_t page_id=row_.GetRowId().GetPageId();
  if(page_id==INVALID_PAGE_ID) return *this;
  auto page=reinterpret_cast<TablePage *>(table_heap_->buffer_pool_manager_->FetchPage(page_id));
  if(page==nullptr) return *this;
  RowId next_rid;
  // 在当前页找下一条记录
  if(page->GetNextTupleRid(row_.GetRowId(),&next_rid)){
    row_.SetRowId(next_rid);
    table_heap_->GetTuple(&row_,txn_);
    table_heap_->buffer_pool_manager_->UnpinPage(page_id, false);
    return *this;
  }
  page_id_t next_page_id=page->GetNextPageId();
  table_heap_->buffer_pool_manager_->UnpinPage(row_.GetRowId().GetPageId(), false);
  // 往后找第一个有有效记录的页
  while(next_page_id!=INVALID_PAGE_ID){
    auto next_page=reinterpret_cast<TablePage *>(table_heap_->buffer_pool_manager_->FetchPage(next_page_id));
    if(next_page==nullptr){
      row_.SetRowId(INVALID_ROWID);
      return *this;
    }
    if(next_page->GetFirstTupleRid(&next_rid)){
      row_.SetRowId(next_rid);
      table_heap_->GetTuple(&row_,txn_);
      table_heap_->buffer_pool_manager_->UnpinPage(next_page_id, false);
      return *this;
    }
    page_id_t next_id=next_page->GetNextPageId();
    table_heap_->buffer_pool_manager_->UnpinPage(next_page_id, false);
    next_page_id=next_id;
  }
  // 没有更多记录了
  row_.SetRowId(INVALID_ROWID);
  return *this;
}

// iter++
TableIterator TableIterator::operator++(int) {
  TableIterator temp(*this);
  ++(*this);
  return temp;
}