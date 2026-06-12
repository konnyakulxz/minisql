#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

frame_id_t BufferPoolManager::TryToFindFreePage(){//找空位
  if(!free_list_.empty()){//缓存中还有空位
    frame_id_t frame_id=free_list_.front();
    free_list_.pop_front();
    return frame_id;
  }
  //缓存没空位，需要根据LRU策略找一个被替换的页
  frame_id_t frame_id;
  if(replacer_->Victim(&frame_id)) return frame_id;
  return INVALID_FRAME_ID;//缓存满了，并且所有页都被固定了，无法替换
}

Page *BufferPoolManager::FetchPage(page_id_t page_id){//在缓存中放该页
  auto it=page_table_.find(page_id);//根据page id找对应的frame id
  if(it!=page_table_.end()){//缓存中有这个页
    frame_id_t frame_id=it->second;
    pages_[frame_id].pin_count_++;
    replacer_->Pin(frame_id);
    return &pages_[frame_id];
  }
  //不在缓存中，需要找一个空位或者被替换的页
  frame_id_t frame_id=TryToFindFreePage();
  if(frame_id==INVALID_FRAME_ID) return nullptr;//缓存满了，并且所有页都被固定了，无法替换
  //替换掉frame_id对应的页
  Page &page=pages_[frame_id];
  if(page.IsDirty()) disk_manager_->WritePage(page.page_id_,page.GetData());//如果dirty则写回
  if(page.page_id_!=INVALID_PAGE_ID) page_table_.erase(page.page_id_);//如果不是空位（即是要被替换的页），就将其删除
  page_table_[page_id]=frame_id;
  disk_manager_->ReadPage(page_id,page.GetData());
  page.page_id_=page_id;
  page.pin_count_=1;
  page.is_dirty_=false;
  return &page;
}

Page *BufferPoolManager::NewPage(page_id_t &page_id){//在缓存中放新页
  page_id_t new_page_id=AllocatePage();
  if(new_page_id==INVALID_PAGE_ID) return nullptr;
  //与FetchPage类似，找一个空位或者被替换的页来放新页
  frame_id_t frame_id=TryToFindFreePage();
  if(frame_id==INVALID_FRAME_ID){
    DeallocatePage(new_page_id);
    return nullptr;
  }
  Page &page=pages_[frame_id];
  if(page.IsDirty()) disk_manager_->WritePage(page.page_id_,page.GetData());
  if(page.page_id_!=INVALID_PAGE_ID) page_table_.erase(page.page_id_);
  page.ResetMemory();//新页数据全置0
  page.page_id_=new_page_id;
  page.pin_count_=1;
  page.is_dirty_=false;
  page_table_[new_page_id]=frame_id;
  page_id=new_page_id;
  return &page;
}

bool BufferPoolManager::DeletePage(page_id_t page_id){//在缓存和磁盘中删除该页
  auto it=page_table_.find(page_id);
  if(it!=page_table_.end()){//在缓存中
    frame_id_t frame_id=it->second;
    Page &page=pages_[frame_id];
    if(page.pin_count_>0) return false;//被固定了，无法删除
    //删除该页
    page_table_.erase(it);
    page.ResetMemory();
    page.page_id_=INVALID_PAGE_ID;
    page.pin_count_=0;
    page.is_dirty_=false;
    free_list_.push_back(frame_id);
  }
  DeallocatePage(page_id);//在磁盘中删除该页
  return true;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id,bool is_dirty){//取消固定该页
  auto it=page_table_.find(page_id);
  if(it==page_table_.end()) return false;
  frame_id_t frame_id=it->second;
  Page &page=pages_[frame_id];
  if(page.pin_count_<=0) return false;
  page.pin_count_--;
  if(is_dirty) page.is_dirty_=true;//如果修改了该页，则标记为dirty
  if(page.pin_count_==0) replacer_->Unpin(frame_id);//如果该页不再被固定，就可以被替换了
  return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id){//将该页写回磁盘
  auto it=page_table_.find(page_id);
  if(it==page_table_.end()) return false;
  frame_id_t frame_id=it->second;
  Page &page=pages_[frame_id];
  disk_manager_->WritePage(page_id,page.GetData());
  page.is_dirty_=false;//被写回后就不再dirty了
  return true;
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}
