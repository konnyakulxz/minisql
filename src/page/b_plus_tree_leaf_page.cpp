#include "page/b_plus_tree_leaf_page.h"

#include <algorithm>

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(RowId))
#define key_off 0
#define val_off GetKeySize()
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/

/**
 * Init method after creating a new leaf page
 * Including set page type, set current size to zero, set page id/parent id, set
 * next page id and set max size
 * 未初始化next_page_id
 */
void LeafPage::Init(page_id_t page_id,page_id_t parent_id,int key_size,int max_size){
  SetPageType(IndexPageType::LEAF_PAGE);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetKeySize(key_size);
  SetMaxSize(max_size);
  SetNextPageId(INVALID_PAGE_ID);
}

/**
 * Helper methods to set/get next page id
 */
page_id_t LeafPage::GetNextPageId() const {
  return next_page_id_;
}

void LeafPage::SetNextPageId(page_id_t next_page_id) {
  next_page_id_ = next_page_id;
  if (next_page_id == 0) {
    LOG(INFO) << "Fatal error";
  }
}

/**
 * Helper method to find the first index i so that pairs_[i].first >= key
 * NOTE: This method is only used when generating index iterator
 * 二分查找
 */
int LeafPage::KeyIndex(const GenericKey *key,const KeyManager &KM){
  //找到满足key[i]>=key的最小的i
  int l,mid,r;
  for(l=0,r=GetSize()-1,mid=(l+r)>>1;l<=r;mid=(l+r)>>1){
    if(KM.CompareKeys(KeyAt(mid),key)>=0) r=mid-1;
    else l=mid+1;
  }
  return l;
}

/*
 * Helper method to find and return the key associated with input "index"(a.k.a
 * array offset)
 */
GenericKey *LeafPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void LeafPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

RowId LeafPage::ValueAt(int index) const {
  return *reinterpret_cast<const RowId *>(pairs_off + index * pair_size + val_off);
}

void LeafPage::SetValueAt(int index, RowId value) {
  *reinterpret_cast<RowId *>(pairs_off + index * pair_size + val_off) = value;
}

void *LeafPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void LeafPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * (GetKeySize() + sizeof(RowId)));
}
/*
 * Helper method to find and return the key & value pair associated with input
 * "index"(a.k.a. array offset)
 */
std::pair<GenericKey *, RowId> LeafPage::GetItem(int index) { return {KeyAt(index), ValueAt(index)}; }

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert key & value pair into leaf page ordered by key
 * @return page size after insertion
 */
int LeafPage::Insert(GenericKey *key,const RowId &value,const KeyManager &KM){
  int pos=KeyIndex(key,KM);
  if(pos<GetSize()&&KM.CompareKeys(KeyAt(pos),key)==0) return GetSize();//key重复，不插入直接返回
  for(int i=GetSize();i>pos;i--) PairCopy(PairPtrAt(i),PairPtrAt(i-1));
  SetKeyAt(pos,key);
  SetValueAt(pos,value);
  IncreaseSize(1);
  return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page
 */
void LeafPage::MoveHalfTo(LeafPage *recipient){
  int half=GetSize()/2;
  int start=GetSize()-half;//将后半部分给recipient
  recipient->CopyNFrom(PairPtrAt(start),half);
  SetSize(start);
}

/*
 * Copy starting from items, and copy {size} number of elements into me.
 */
void LeafPage::CopyNFrom(void *src,int size){
  int start=GetSize();//追加到末尾
  PairCopy(PairPtrAt(start),src,size);
  SetSize(start+size);
}

/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * For the given key, check to see whether it exists in the leaf page. If it
 * does, then store its corresponding value in input "value" and return true.
 * If the key does not exist, then return false
 */
bool LeafPage::Lookup(const GenericKey *key,RowId &value,const KeyManager &KM){
  int pos=KeyIndex(key,KM);
  if(pos<GetSize()&&KM.CompareKeys(KeyAt(pos),key)==0){//exists
    value=ValueAt(pos);
    return true;
  }
  return false;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * First look through leaf page to see whether delete key exist or not. If
 * existed, perform deletion, otherwise return immediately.
 * NOTE: store key&value pair continuously after deletion
 * @return  page size after deletion
 */
int LeafPage::RemoveAndDeleteRecord(const GenericKey *key,const KeyManager &KM){
  int pos=KeyIndex(key,KM);
  if(pos>=GetSize()||KM.CompareKeys(KeyAt(pos),key)!=0) return GetSize();//key不存在
  for(int i=pos;i<GetSize()-1;i++) PairCopy(PairPtrAt(i),PairPtrAt(i+1));
  IncreaseSize(-1);
  return GetSize();
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all key & value pairs from this page to "recipient" page. Don't forget
 * to update the next_page id in the sibling page
 */
void LeafPage::MoveAllTo(LeafPage *recipient){
  recipient->CopyNFrom(PairPtrAt(0),GetSize());
  recipient->SetNextPageId(GetNextPageId());
  SetSize(0);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to "recipient" page.
 *
 */
void LeafPage::MoveFirstToEndOf(LeafPage *recipient){//左边向右边借
  recipient->CopyLastFrom(KeyAt(0),ValueAt(0));
  for(int i=0;i<GetSize()-1;i++) PairCopy(PairPtrAt(i),PairPtrAt(i+1));
  IncreaseSize(-1);
}

/*
 * Copy the item into the end of my item list. (Append item to my array)
 */
void LeafPage::CopyLastFrom(GenericKey *key,RowId value){
  SetKeyAt(GetSize(),key);
  SetValueAt(GetSize(),value);
  IncreaseSize(1);
}

/*
 * Remove the last key & value pair from this page to "recipient" page.
 */
void LeafPage::MoveLastToFrontOf(LeafPage *recipient){//右边向左边借
  int last=GetSize()-1;
  recipient->CopyFirstFrom(KeyAt(last),ValueAt(last));
  IncreaseSize(-1);
}

/*
 * Insert item at the front of my items. Move items accordingly.
 *
 */
void LeafPage::CopyFirstFrom(GenericKey *key,RowId value){
  for(int i=GetSize();i>0;i--) PairCopy(PairPtrAt(i),PairPtrAt(i-1));
  SetKeyAt(0,key);
  SetValueAt(0,value);
  IncreaseSize(1);
}