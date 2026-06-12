#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

BPlusTree::BPlusTree(index_id_t index_id,BufferPoolManager *buffer_pool_manager,const KeyManager &KM,
                     int leaf_max_size,int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  int key_size=processor_.GetKeySize();
  if(leaf_max_size_==UNDEFINED_SIZE) leaf_max_size_=(PAGE_SIZE-LEAF_PAGE_HEADER_SIZE)/(key_size+sizeof(RowId));
  if(internal_max_size_==UNDEFINED_SIZE) internal_max_size_=(PAGE_SIZE-INTERNAL_PAGE_HEADER_SIZE)/(key_size+sizeof(page_id_t));
}

void BPlusTree::Destroy(page_id_t current_page_id){//销毁以current为根的整棵树
  if(IsEmpty()) return;
  if(current_page_id==INVALID_PAGE_ID) current_page_id=root_page_id_;
  auto page=buffer_pool_manager_->FetchPage(current_page_id);
  if(page==nullptr) return;
  auto *node=reinterpret_cast<BPlusTreePage *>(page->GetData());
  if(!node->IsLeafPage()){//当前为内部节点，则递归删除子节点
    auto *inner=reinterpret_cast<InternalPage *>(node);
    for(int i=0;i<inner->GetSize();i++) Destroy(inner->ValueAt(i));
  }
  buffer_pool_manager_->UnpinPage(current_page_id,false);
  buffer_pool_manager_->DeletePage(current_page_id);
  if(current_page_id==root_page_id_) root_page_id_=INVALID_PAGE_ID;
}

bool BPlusTree::IsEmpty() const{
  return root_page_id_==INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key,std::vector<RowId> &result,Txn *transaction){
  if(IsEmpty()) return false;
  auto leaf_page=reinterpret_cast<LeafPage *>(FindLeafPage(key));//注意该函数是有fetch过的，需要unpin
  RowId value;
  if(leaf_page->Lookup(key,value,processor_)){//查到key
    result.push_back(value);
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),false);
    return true;
  }
  //查不到
  buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),false);
  return false;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key,const RowId &value,Txn *transaction){
  if(IsEmpty()){
    StartNewTree(key,value);
    UpdateRootPageId(1);
    return true;
  }
  return InsertIntoLeaf(key,value,transaction);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key,const RowId &value){
  page_id_t page_id;
  auto page=buffer_pool_manager_->NewPage(page_id);
  if(page==nullptr) throw std::runtime_error("out of memory");
  auto *leaf=reinterpret_cast<LeafPage *>(page->GetData());//新树的root是leaf
  leaf->Init(page_id,INVALID_PAGE_ID,processor_.GetKeySize(),leaf_max_size_);
  leaf->Insert(key,value,processor_);
  root_page_id_=page_id;
  buffer_pool_manager_->UnpinPage(page_id,true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key,const RowId &value,Txn *transaction){
  auto leaf_page=reinterpret_cast<LeafPage *>(FindLeafPage(key));
  int old_size=leaf_page->GetSize();
  int new_size=leaf_page->Insert(key,value,processor_);
  if(new_size==old_size){//key已存在
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),false);
    return false;
  }
  if(new_size>leaf_page->GetMaxSize()){//需要分裂
    auto new_leaf=Split(leaf_page,transaction);//注意这里隐含了pin
    InsertIntoParent(leaf_page,new_leaf->KeyAt(0),new_leaf,transaction);//上报父节点
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),true);
    buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(),true);
  }
  else buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),true);
  return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
LeafPage *BPlusTree::Split(LeafPage *node,Txn *transaction){//叶节点的split
  page_id_t new_page_id;
  auto new_page=buffer_pool_manager_->NewPage(new_page_id);
  if(new_page==nullptr) throw std::runtime_error("out of memory");
  auto *new_leaf=reinterpret_cast<LeafPage *>(new_page->GetData());
  new_leaf->Init(new_page_id,node->GetParentPageId(),processor_.GetKeySize(),leaf_max_size_);
  node->MoveHalfTo(new_leaf);
  new_leaf->SetNextPageId(node->GetNextPageId());//更新叶子链表
  node->SetNextPageId(new_page_id);
  return new_leaf;//调用者负责unpin
}

InternalPage *BPlusTree::Split(InternalPage *node,Txn *transaction){//内部节点的split
  page_id_t new_page_id;
  auto new_page=buffer_pool_manager_->NewPage(new_page_id);
  if(new_page==nullptr) throw std::runtime_error("out of memory");
  auto *new_internal=reinterpret_cast<InternalPage *>(new_page->GetData());
  new_internal->Init(new_page_id,node->GetParentPageId(),processor_.GetKeySize(),internal_max_size_);
  node->MoveHalfTo(new_internal,buffer_pool_manager_);//内部节点不用更新链表
  return new_internal;//调用者负责unpin
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node,GenericKey *key,BPlusTreePage *new_node,Txn *transaction){
  if(old_node->IsRootPage()){//根
    page_id_t new_root_id;
    auto new_root_page=buffer_pool_manager_->NewPage(new_root_id);
    if(new_root_page==nullptr) throw std::runtime_error("out of memory");
    auto *new_root=reinterpret_cast<InternalPage *>(new_root_page->GetData());
    new_root->Init(new_root_id,INVALID_PAGE_ID,processor_.GetKeySize(),internal_max_size_);
    new_root->PopulateNewRoot(old_node->GetPageId(),key,new_node->GetPageId());//更新new root相关信息
    old_node->SetParentPageId(new_root_id);
    new_node->SetParentPageId(new_root_id);
    root_page_id_=new_root_id;
    UpdateRootPageId();//每次根变了都要调用这个函数
    buffer_pool_manager_->UnpinPage(new_root_id,true);
    return;
  }
  //非根
  page_id_t parent_id=old_node->GetParentPageId();
  auto parent_page=buffer_pool_manager_->FetchPage(parent_id);
  auto *parent=reinterpret_cast<InternalPage *>(parent_page->GetData());
  parent->InsertNodeAfter(old_node->GetPageId(),key,new_node->GetPageId());//在old page后面插入new page和key
  new_node->SetParentPageId(parent_id);
  if(parent->GetSize()>parent->GetMaxSize()){//父节点也要分裂
    auto new_parent=Split(parent,transaction);
    InsertIntoParent(parent,new_parent->KeyAt(0),new_parent,transaction);//分裂+上报
    buffer_pool_manager_->UnpinPage(new_parent->GetPageId(),true);
  }
  buffer_pool_manager_->UnpinPage(parent_id,true);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key,Txn *transaction){
  if(IsEmpty()) return;
  auto leaf_page=reinterpret_cast<LeafPage *>(FindLeafPage(key));
  int old_size=leaf_page->GetSize();
  leaf_page->RemoveAndDeleteRecord(key,processor_);
  if(old_size==leaf_page->GetSize()){//key不存在
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),false);
    return;
  }
  bool should_delete=CoalesceOrRedistribute(leaf_page,transaction);
  buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),true);
  if(should_delete) buffer_pool_manager_->DeletePage(leaf_page->GetPageId());
}

/*
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node,Txn *transaction){
  if(node->GetSize()>=node->GetMinSize()) return false;//删除后仍够大,不需要处理
  if(node->IsRootPage()) return AdjustRoot(node);
  auto parent_page=buffer_pool_manager_->FetchPage(node->GetParentPageId());
  auto *parent=reinterpret_cast<InternalPage *>(parent_page->GetData());
  int index=parent->ValueIndex(node->GetPageId());//根据page id找到index
  int neighbor_index=(index==0)?1:index-1;//找到左邻居
  page_id_t neighbor_id=parent->ValueAt(neighbor_index);
  auto neighbor_page=buffer_pool_manager_->FetchPage(neighbor_id);
  auto *neighbor=reinterpret_cast<N *>(neighbor_page->GetData());

  if(node->GetSize()+neighbor->GetSize()<=node->GetMaxSize()){//合并
    bool parent_should_delete=Coalesce(neighbor,node,parent,index,transaction);
    buffer_pool_manager_->UnpinPage(neighbor_id,true);
    buffer_pool_manager_->UnpinPage(parent_id,true);
    if(parent_should_delete) buffer_pool_manager_->DeletePage(parent_id);
    return true;
  }
  else{//重分配
    Redistribute(neighbor,node,index);
    buffer_pool_manager_->UnpinPage(neighbor_id,true);
    buffer_pool_manager_->UnpinPage(parent_id,true);
    return false;
  }
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node,LeafPage *&node,InternalPage *&parent,int index,//叶节点
                         Txn *transaction){
  if(index==0){//此时node在左边，neighbor在右边，swap使neighbor始终在左边
    std::swap(node,neighbor_node);
    index=1;
  }
  node->MoveAllTo(neighbor_node);//将node的所有数据移到neighbor_node
  parent->Remove(index);//从父节点中删除node对应的pair
  return CoalesceOrRedistribute(parent,transaction);//父节点被修改了，可能还得合并/重分配
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node,InternalPage *&node,InternalPage *&parent,int index,//内部节点
                         Txn *transaction){
  if(index==0){
    std::swap(node,neighbor_node);
    index=1;
  }
  GenericKey *middle_key=parent->KeyAt(index);
  node->MoveAllTo(neighbor_node,middle_key,buffer_pool_manager_);//合并后node中原本空的key0现在需要存放信息了
  parent->Remove(index);
  return CoalesceOrRedistribute(parent,transaction);
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node,LeafPage *node,int index){
  if(index==0) neighbor_node->MoveFirstToEndOf(node);//node在左边，从右边借
  else neighbor_node->MoveLastToFrontOf(node);//node在右边，从左边借
}

void BPlusTree::Redistribute(InternalPage *neighbor_node,InternalPage *node,int index){
  auto parent_page=buffer_pool_manager_->FetchPage(node->GetParentPageId());
  auto *parent=reinterpret_cast<InternalPage *>(parent_page->GetData());
  auto *saved_key=new char[processor_.GetKeySize()];
  if(index==0){//node在左边，从右边借
    GenericKey *middle_key=parent->KeyAt(1);
    memcpy(saved_key,neighbor_node->KeyAt(1),processor_.GetKeySize());
    neighbor_node->MoveFirstToEndOf(node,middle_key,buffer_pool_manager_);
    parent->SetKeyAt(1,reinterpret_cast<GenericKey *>(saved_key));//parent key1是旧的neighbor key1
  }else{//node在右边，从左边借
    GenericKey *middle_key=parent->KeyAt(index);
    memcpy(saved_key,neighbor_node->KeyAt(neighbor_node->GetSize()-1),processor_.GetKeySize());
    neighbor_node->MoveLastToFrontOf(node,middle_key,buffer_pool_manager_);
    parent->SetKeyAt(index,reinterpret_cast<GenericKey *>(saved_key));//parent key[index]是旧的neighbor key[size-1]
  }
  delete[] saved_key;
  buffer_pool_manager_->UnpinPage(parent->GetPageId(),true);
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node){
  if(old_root_node->IsLeafPage()){//根是叶子
    if(old_root_node->GetSize()==0){//树为空
      root_page_id_=INVALID_PAGE_ID;
      UpdateRootPageId();
      return true;
    }
    return false;
  }
  //根是内部节点
  auto *root=reinterpret_cast<InternalPage *>(old_root_node);
  if(root->GetSize()==1){//只有一个孩子，让它成为新根
    page_id_t child_id=root->RemoveAndReturnOnlyChild();
    auto child_page=buffer_pool_manager_->FetchPage(child_id);
    auto *child_node=reinterpret_cast<BPlusTreePage *>(child_page->GetData());
    child_node->SetParentPageId(INVALID_PAGE_ID);
    root_page_id_=child_id;
    UpdateRootPageId();
    buffer_pool_manager_->UnpinPage(child_id,true);
    return true;
  }
  return false;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(){
  if(IsEmpty()) return End();
  auto leaf=reinterpret_cast<LeafPage *>(FindLeafPage(nullptr,INVALID_PAGE_ID,true));
  page_id_t leaf_id=leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(leaf_id,false);
  return IndexIterator(leaf_id,buffer_pool_manager_,0);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key){
  if(IsEmpty()) return End();
  auto leaf=reinterpret_cast<LeafPage *>(FindLeafPage(key));
  int pos=leaf->KeyIndex(key,processor_);
  page_id_t leaf_id=leaf->GetPageId();
  if(pos==leaf->GetSize()){//当前page中不存在大等于key的键，说明第一个大等于key的键在下一个page中
    page_id_t next_id=leaf->GetNextPageId();
    buffer_pool_manager_->UnpinPage(leaf_id,false);
    if(next_id==INVALID_PAGE_ID) return End();//key比这棵树中所有的键都大
    return IndexIterator(next_id,buffer_pool_manager_,0);
  }
  buffer_pool_manager_->UnpinPage(leaf_id,false);
  return IndexIterator(leaf_id,buffer_pool_manager_,pos);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End(){
  return IndexIterator();
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key,page_id_t page_id,bool leftMost){
  if(IsEmpty()) return nullptr;
  if(page_id==INVALID_PAGE_ID) page_id=root_page_id_;
  auto page=buffer_pool_manager_->FetchPage(page_id);
  auto *node=reinterpret_cast<BPlusTreePage *>(page->GetData());
  while(!node->IsLeafPage()){
    auto *inner=reinterpret_cast<InternalPage *>(node);
    page_id_t child_id;
    if(leftMost) child_id=inner->ValueAt(0);//找最左边的leaf
    else child_id=inner->Lookup(key,processor_);
    buffer_pool_manager_->UnpinPage(node->GetPageId(),false);
    page=buffer_pool_manager_->FetchPage(child_id);
    node=reinterpret_cast<BPlusTreePage *>(page->GetData());
  }
  return page;
}

/*
 * Update/Insert root page id in header page(where page_id = INDEX_ROOTS_PAGE_ID,
 * header_page isdefined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record){
  auto header_page=buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
  auto *roots=reinterpret_cast<IndexRootsPage *>(header_page->GetData());
  if(insert_record) roots->Insert(index_id_,root_page_id_);
  else if(root_page_id_==INVALID_PAGE_ID) roots->Delete(index_id_);
  else roots->Update(index_id_,root_page_id_);
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID,true);
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row ans;
      processor_.DeserializeToKey(leaf->KeyAt(i), ans, schema);
      out << "<TD>" << ans.GetField(0)->toString() << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        Row ans;
        processor_.DeserializeToKey(inner->KeyAt(i), ans, schema);
        out << ans.GetField(0)->toString();
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out, schema);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}