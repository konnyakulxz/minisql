#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) {}

LRUReplacer::~LRUReplacer() = default;

bool LRUReplacer::Victim(frame_id_t *frame_id){//替换
  if(lru_list.empty()) return false;
  *frame_id=lru_list.front();
  lru_map.erase(*frame_id);
  lru_list.pop_front();
  return true;
}

void LRUReplacer::Pin(frame_id_t frame_id){//固定
  auto it=lru_map.find(frame_id);
  if(it!=lru_map.end()){//该页在lru list中，需要删除
    lru_list.erase(it->second);
    lru_map.erase(it);
  }
}

void LRUReplacer::Unpin(frame_id_t frame_id){//取消固定
  if(lru_map.find(frame_id)!=lru_map.end()) return;//该页本来就没被固定
  lru_list.push_back(frame_id);
  lru_map[frame_id]=std::prev(lru_list.end());//prev返回最后一个元素的迭代器
}

size_t LRUReplacer::Size(){
  return lru_list.size();
}