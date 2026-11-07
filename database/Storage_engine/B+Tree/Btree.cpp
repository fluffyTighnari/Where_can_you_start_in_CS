/*
 * Btree.cpp —— B+ 树节点操作与树级算法实现
 *
 * 文件分为三段：
 *   1. 节点内操作（Btree_Node::*）：二分查找、插入/删除键与值、分裂、借位、合并
 *   2. 树级操作（Btree_*）：find / scan / insert / update / delete / delete_range
 *   3. 级联处理（Split_chain / Merge_chain）：沿 StackPath 向上传播结构变化
 *
 * 重要不变量（validate() 中检查）：
 *   - 每个节点 key_number <= MAX_KEYS
 *   - 叶子节点 offsets[0] == 0，offsets 严格递增，curval 不超 values 容量
 *   - 同层节点的键严格递增
 *   - 中间节点 son_pages[0..key_number] 共 key_number+1 个子指针
 */
#include "Btree.hpp"
#include "StackPath.hpp"
#include <cstring>
#include <memory>
#include <stack>
#include <iostream>

/* 二分查找：返回第一个 keys[i] >= key 的下标；全小于 key 时返回 key_number */
uint32_t Btree_Node::Find_Key_Index(const Key& key) const{/*工具函数*/
    if(key_number == 0){
        return 0;
    }

    int left = 0, right = key_number - 1;
    while(left <= right){
        int mid = left + (right - left)/2;
        if(keys[mid] < key){
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return left;
}

/*
 * 中间节点路由：找到 key 应走哪个子页。
 * 若 key 恰好等于某个分隔键，走它右侧的子页（son_pages[index+1]），
 * 保证等于分界值的记录落在右子树，与插入时的上浮规则保持一致。
 */
uint32_t Btree_Node::Find_Next_Page(const Key& key) const{
    if(is_leaf()){
        return 0;
    }
    int index = Find_Key_Index(key);
    if(index < key_number && keys[index] == key){
        return son_pages[index + 1];
    }
    return son_pages[index];
}

/*
 * 叶子节点按键查找值。
 * 值的长度由后一个 key 的偏移推出（最后一个用 curval），这是本实现紧凑存储的关键。
 */
bool Btree_Node::Find_Value(const Key& key,char* buffer, uint32_t& out_size) const{
    if(!is_leaf() || !buffer){
        return false;
    }
    int index = Find_Key_Index(key);

    if(index >= key_number || keys[index] != key){
        return false;
    }

    uint32_t val_size;
    if(index == key_number - 1){
        val_size = curval - offsets[index];
    }
    else{
        val_size = offsets[index + 1] - offsets[index];
    }
    memcpy(buffer, values + offsets[index], val_size);
    out_size = val_size;
    return true;
}

/* 把整个 values 区追加到 buffer 的指定偏移处，返回拷贝的字节数（scan 用） */
uint32_t Btree_Node::Find_Value(char* buffer, uint32_t buf_offset) const{
    if(!is_leaf() || !buffer || key_number == 0){
        return 0;
    }

    memcpy(buffer + buf_offset, values, curval);
    return curval;
}

/*
 * 中间节点插入键：从后往前腾位置，把 keys 与 son_pages 一起右移。
 * 新键 key 位于 index 位置，其右侧子指针为 son_page（左侧沿用原有的 son_pages[index]）。
 */
bool Btree_Node::Insert_Key(const Key& key,uint32_t son_page){
    if(is_leaf() || key_number == MAX_KEYS){
        return false;
    }
    int index = Find_Key_Index(key);

    if(keys[index] == key){
        return false;
    }

    for(uint32_t i = key_number;i > index;i--){
        keys[i] = keys[i - 1];
        son_pages[i + 1] = son_pages[i];
    }
    keys[index] = key;
    son_pages[index + 1] = son_page;
    key_number++;
    
    dirty = true;
    return true;
}

/*
 * 叶子节点插入一行数据。要点：
 *   1. values 是紧凑拼接的，插入点之后的字节整体后移 val_size（memmove 处理重叠）
 *   2. 插入点之后的 offsets 全部要加上 val_size，因为它们的值被推后了
 *   3. 新键的 offset 记录为插入点，key_number 与 curval 同步增加
 */
bool Btree_Node::Insert_Value(const Key& key, const char* buffer, uint32_t val_size) {
    if (!is_leaf() || key_number >= MAX_KEYS || !buffer || val_size == 0) return false;
    if (curval + val_size > sizeof(values)) return false;

    int index = Find_Key_Index(key);
    if(index < key_number && keys[index] == key){
        return false;
    }

    uint32_t insert_offset = (index == key_number) ? curval : offsets[index];

    if(index < key_number){
        uint32_t move_len = curval - insert_offset;
        memmove(values + insert_offset + val_size, values + insert_offset, move_len);
    }

    for(int i = key_number; i > index; --i){
        keys[i] = keys[i - 1];
        offsets[i] = offsets[i - 1];
    }

    for(int i = index + 1; i <= key_number; ++i){
        offsets[i] += val_size;
    }

    keys[index] = key;
    memcpy(values + insert_offset, buffer, val_size);
    offsets[index] = insert_offset;

    key_number++;
    curval += val_size;
    dirty = true;
    return true;
}

/*
 * 原地更新某键对应的值，新值长度可以与旧值不同。
 * 长度变化时把后续字节整体前移/后移，并同步调整受影响区间的 offsets。
 * 空间不足时直接返回 false，由上层决定是否换一种策略。
 */
bool Btree_Node::Update_Value(const Key& key,const char* buffer, uint32_t val_size){
    if(!is_leaf() || !buffer || val_size == 0){
        return false;
    }
    int index = Find_Key_Index(key);

    if(index >= key_number || keys[index] != key){
        return false;
    }

    uint32_t old_size;
    if(index == key_number - 1){
        old_size = curval - offsets[index];
    }
    else{
        old_size = offsets[index + 1] - offsets[index];
    }

    if (val_size > old_size && curval + (val_size - old_size) > sizeof(values)) {
        return false;
    }

    int change = static_cast<int>(old_size) - static_cast<int>(val_size);

    if(index != key_number - 1){
        memmove(values + offsets[index] + val_size, values + offsets[index] + old_size, curval - (offsets[index] + old_size));
        for(uint32_t i = index + 1; i < key_number; i++){
            offsets[i] -= change;
        }
    }

    memcpy(values + offsets[index], buffer, val_size);
    curval -= change;

    dirty = true;
    return true;
}

/*
 * 中间节点删键：需要同时判断待删子页是键左侧还是右侧，
 * 据此确定要移除的是 son_pages[index] 还是 son_pages[index+1]。
 */
bool Btree_Node::Delete_Key(const Key& key,uint32_t son_page){
    if(is_leaf()){
        return false;
    }
    int index = Find_Key_Index(key);

    if(keys[index] != key){
        return false;
    }

    uint32_t son_idx;
    if(son_pages[index] == son_page){
        son_idx = index;
    } else if(son_pages[index + 1] == son_page){
        son_idx = index + 1;
    } else {
        return false;
    }

    for(uint32_t i = index; i < key_number - 1; i++){
        keys[i] = keys[i + 1];
    }

    for(uint32_t i = son_idx; i < key_number; i++){
        son_pages[i] = son_pages[i + 1];
    }

    key_number--;
    dirty = true;
    return true;
}

/* 替换中间节点的分隔键（借位后父节点的分界值需要跟着变） */
bool Btree_Node::Update_Key(const Key& prekey,const Key& newkey){
    int index = Find_Key_Index(prekey);

    if(keys[index] != prekey){
        return false;
    }

    keys[index] = newkey;

    dirty = true;
    return true;
}

/*
 * 叶子节点删除一行：把该值的字节从 values 中挖掉（memmove 前移），
 * 同步把后续 keys 左移、后续 offsets 减去被删长度。
 * 若删的是最后一个键，只需缩短 curval，无需搬移。
 */
bool Btree_Node::Delete_Value(const Key& key){
    if(!is_leaf()){
        return false;
    }
    int index = Find_Key_Index(key);

    if(keys[index] != key){
        return false;
    }
    uint32_t val_size;

    if(index != key_number - 1){
        val_size = offsets[index + 1] - offsets[index];
        memmove(values + offsets[index],values + offsets[index + 1],curval - offsets[index + 1]);
        for(uint32_t i = index;i < key_number - 1;i++){
            keys[i] = keys[i + 1];
            if(i != index){
                offsets[i] = offsets[i + 1] - val_size;
            }
        }
    }
    else{
        val_size = curval - offsets[index];
    }
    key_number--;
    curval -= val_size;

    dirty = true;
    return true;
}

/*
 * 分裂：把节点后半部分的键（以及叶子节点的值）搬到新节点，自身保留前半部分。
 * 分界键 risekey = new_page.keys[0]，由调用方插入到父节点。
 *
 * 叶子节点需要搬移 values 并重算 offsets（减去基准偏移 move_offset）；
 * 中间节点的 son_pages 比 keys 多一个，末尾的子指针要单独搬。
 */
bool Btree_Node::Page_Split(Btree_Node& new_page,uint32_t new_page_num,Key& risekey){
    if(key_number < 2){
        return false;
    }

    int mid = key_number / 2;

    new_page.dirty = true;
    new_page.father = father;
    new_page.root = false;
    new_page.key_number = key_number - mid;
    new_page.type = type;
    new_page.page_num = new_page_num;
    new_page.curval = 0;
    new_page.pre_leaf = page_num;
    new_page.next_leaf = next_leaf;
    next_leaf = new_page_num;

    if(type == Btree_Node::Type::LEAF_NODE){

        for(uint32_t i = 0; i < new_page.key_number; i++){
            new_page.keys[i] = keys[mid + i];
            new_page.offsets[i] = offsets[mid + i] - offsets[mid];
        }
        uint32_t move_offset = offsets[mid];
        new_page.curval = curval - move_offset;
        memcpy(new_page.values, values + move_offset, new_page.curval);

        key_number = mid;
        curval = move_offset;
        values[curval] = '\0';
    } else {
        for(uint32_t i = 0; i < new_page.key_number; i++){
            new_page.keys[i] = keys[mid + i];
            new_page.son_pages[i] = son_pages[mid + i];
        }

        new_page.son_pages[new_page.key_number] = son_pages[key_number];
        key_number = mid;
    }

    risekey = new_page.keys[0];

    dirty = true;
    return true;
}

/*
 * 向左兄弟借一个键。
 * 前提：左兄弟的键数 > MAX_KEYS/2（借完不会让它欠载）。
 * 叶子节点还要搬对应的值字节，并整体右移本节点的 offsets。
 */
bool Btree_Node::Borrow_Left(Btree_Node& left){
    if(left.key_number <= MAX_KEYS / 2){
        return false;
    }
    if(type == Btree_Node::Type::LEAF_NODE){
        int last_key_idx = left.key_number - 1;
        int size = left.curval - left.offsets[last_key_idx];

        for(int i = key_number; i > 0; i--){
            keys[i] = keys[i - 1];
            offsets[i] = offsets[i - 1] + size;
        }

        keys[0] = left.keys[last_key_idx];

        memmove(values + size,values,curval);

        memcpy(values,left.values + left.offsets[last_key_idx],size);

        left.key_number--;
        key_number++;
        left.curval -= size;
        curval += size;
    }else{
        int last_key_idx = left.key_number - 1;
        Key borrowed_key = left.keys[last_key_idx];
        uint32_t borrowed_son = left.son_pages[last_key_idx + 1];

        for(int i = key_number; i > 0; i--){
            keys[i] = keys[i - 1];
        }
        for(int i = key_number + 1; i > 1; i--){
            son_pages[i] = son_pages[i - 1];
        }
        keys[0] = borrowed_key;
        son_pages[1] = borrowed_son;

        left.key_number--;
        key_number++;
    }
    
    dirty = true;
    left.dirty = true;
    return true;
}

/* 向右兄弟借一个键：把右兄弟的第一个键/值追加到本节点末尾 */
bool Btree_Node::Borrow_Right(Btree_Node& right){
    if(right.key_number <= MAX_KEYS / 2){
        return false;
    }
    if(type == Btree_Node::Type::LEAF_NODE){
        uint32_t size = right.offsets[1] - right.offsets[0];
        keys[key_number] = right.keys[0];
        offsets[key_number] = curval;

        memcpy(values + curval, right.values, size);

        for(uint32_t i = 0; i < right.key_number - 1; i++){
            right.keys[i] = right.keys[i + 1];
            right.offsets[i] = right.offsets[i + 1] - size;
        }
        memmove(right.values,right.values + size,right.curval - size);
        
        right.key_number--;
        key_number++;
        right.curval -= size;
        curval += size;
    }else{
        keys[key_number] = right.keys[0];
        son_pages[key_number + 1] = right.son_pages[1];

        for (uint32_t i = 0; i < right.key_number - 1; i++) {
            right.keys[i] = right.keys[i + 1];
        }
        for (uint32_t i = 1; i < right.key_number; i++) {
            right.son_pages[i] = right.son_pages[i + 1];
        }

        right.key_number--;
        key_number++;
    }

    dirty = true;
    right.dirty = true;
    return true;
}

/*
 * 与左兄弟合并：把自身全部内容接到左兄弟末尾，自身被清空。
 * delkey 输出父节点中应删除的分隔键（本节点原来的最小键）。
 * 叶子合并后要修正前驱指针，保证叶子链表不断裂。
 *
 * 中间节点合并时，左兄弟的键整体右移 mid 个位置给本节点的键腾出空间。
 */
bool Btree_Node::Merge_Left(Btree_Node& left,Key& delkey){
    if(left.father != father || !(key_number == MAX_KEYS / 2 && left.key_number >= MAX_KEYS / 2)){
        return false;
    }
    delkey = keys[0];

    int mid = MAX_KEYS / 2;
    if(type == Btree_Node::Type::LEAF_NODE){
        uint32_t left_kn = left.key_number;

        for(uint32_t i = key_number; i > 0; i--){
            keys[left_kn + i - 1] = keys[i - 1];
            offsets[left_kn + i - 1] = offsets[i - 1] + left.curval;
        }
        for(uint32_t i = 0; i < left_kn; i++){
            keys[i] = left.keys[i];
            offsets[i] = left.offsets[i];
        }

        memmove(values + left.curval, values, curval);
        memcpy(values, left.values, left.curval);

        key_number += left_kn;
        curval += left.curval;
        left.key_number = 0;
        left.curval = 0;

        pre_leaf = left.pre_leaf;
    }else{
        for(uint32_t i = MAX_KEYS - 1;i >= mid;i--){
            keys[i] = keys[i - mid];
            son_pages[i + 1] = son_pages[i + 1 - mid];
        }

        for(uint32_t i = 0;i < mid;i++){
            keys[i] = left.keys[i];
            son_pages[i] = left.son_pages[i];
        }
        son_pages[mid] = left.son_pages[mid];

        left.key_number -= mid;
        key_number += mid;
    }

    dirty = true;
    return true;
}

/* 与右兄弟合并：把右兄弟的前半部分搬进本节点，右兄弟被清空；delkey 为其最小键 */
bool Btree_Node::Merge_Right(Btree_Node& right,Key& delkey){
    if(right.father != father || !(key_number == MAX_KEYS / 2 && right.key_number >= MAX_KEYS / 2)){
        return false;
    }
    delkey = right.keys[0];

    int mid = MAX_KEYS / 2;
    if(type == Btree_Node::Type::LEAF_NODE){
        for(uint32_t i = 0;i < mid;i++){
            keys[mid + i] = right.keys[i];
            offsets[mid + i] = curval + right.offsets[i];
        }

        memcpy(values + curval,right.values,right.curval);

        key_number += mid;
        right.key_number -= mid;
        curval += right.curval;
        right.curval = 0;

        next_leaf = right.next_leaf;
    }else{
        for(uint32_t i = 0;i < mid;i++){
            keys[mid + i] = right.keys[i];
            son_pages[mid + i] = right.son_pages[i];
        }
        son_pages[MAX_KEYS] = right.son_pages[mid];

        key_number += mid;
        right.key_number -= mid;
    }

    dirty = true;
    return true;
}

/* 清空根节点内容，用于 delete_range 保留根页、丢弃所有数据 */
bool Btree_Node::clear(){
    if(!is_root()){
        return false;
    }

    memset(keys,0,sizeof(keys));
    key_number = 0;

    if(type == Type::LEAF_NODE){
        memset(values,0,sizeof(values));
        memset(offsets,0,sizeof(offsets));
        curval = 0;

    }
    else{
        memset(son_pages,0,sizeof(son_pages));
    }

    dirty = true;
    return true;
}


/*
 * 精确查找：从根一路向下到叶子，返回该键对应的值。
 * 键不存在时返回 "invalid key" 错误——上层（executor）会据此判定为"无匹配行"。
 */
Result<std::string> Btree_find(uint32_t page_num,Key& key,Pager_t& pager){
    auto Node = std::make_unique<Btree_Node>();
    Btree_Node* buffer = &*Node;
    if(!pager.read_page(page_num,reinterpret_cast<char*>(buffer))){
        return Error("failed to read page",Error::Layer::Pager);
    }

    while(buffer->type != Btree_Node::Type::LEAF_NODE){
        uint32_t next_page = buffer->Find_Next_Page(key);
        if(!pager.read_page(next_page,reinterpret_cast<char*>(buffer))){
            return Error("failed to read page",Error::Layer::Pager);
        }
    }

    std::string result;
    result.resize(LOGNEST_MESSAGE);
    uint32_t out_size = 0;
    if(!buffer->Find_Value(key, &result[0], out_size)){
        return Error("invalid key",Error::Layer::Btree);
    }
    result.resize(out_size);
    return result;
}

/*
 * 全量扫描：先沿最左子指针下到最左叶子，再沿 next_leaf 遍历整条叶子链表，
 * 把每页的 values 区依次拼接。结果天然按主键升序。
 */
Result<std::string> Btree_scan(uint32_t page_num,Pager_t& pager){
    auto Node = std::make_unique<Btree_Node>();
    Btree_Node* buffer = &*Node;
    if(!pager.read_page(page_num,reinterpret_cast<char*>(buffer))){
        return Error("failed to read page",Error::Layer::Pager);
    }
    while(buffer->type != Btree_Node::Type::LEAF_NODE){
        uint32_t next_page = buffer->son_pages[0];
        if(!pager.read_page(next_page,reinterpret_cast<char*>(buffer))){
            return Error("failed to read page",Error::Layer::Pager);
        }
    }

    std::string result;

    result.append(buffer->values, buffer->curval);

    while(buffer->next_leaf != 0){
        if(!pager.read_page(buffer->next_leaf,reinterpret_cast<char*>(buffer))){
            return Error("failed to read page",Error::Layer::Pager);
        }
        result.append(buffer->values, buffer->curval);
    }

    return result;
}

/*
 * 插入主流程：
 *   1. 沿路径下探，把每个节点压入 StackPath
 *   2. 叶子还有空间且键不重复 → 直接插入，结束
 *   3. 否则触发分裂：Split_chain 逐层向上处理
 *   4. 分裂后要重新判断 key 该落在左半还是右半（分界键可能已经把 key 挤到新节点）
 *
 * out_root_page 在根分裂时被更新为新根页号，调用方据此更新符号表。
 * new_pages 中返回本次新建的节点，由调用方统一落盘。
 */
Result<bool> Btree_insert(uint32_t page_num,Key& key,std::string& value,Pager_t& pager,StackPath& path,std::vector<std::unique_ptr<Btree_Node>>& new_pages,uint32_t& out_root_page){
    out_root_page = page_num;
    auto Node = std::make_unique<Btree_Node>();
    if(!pager.read_page(page_num,reinterpret_cast<char*>(&*Node))){
        return Error("failed to read page",Error::Layer::Pager);
    }
    path.push(std::move(Node));
    Btree_Node* temp = &path.top();

    while(temp->type != Btree_Node::Type::LEAF_NODE){
        uint32_t next_page = temp->Find_Next_Page(key);

        if(next_page == 0){
            return Error("bad structure",Error::Layer::Btree);
        }

        auto Node = std::make_unique<Btree_Node>();
        if(!pager.read_page(next_page,reinterpret_cast<char*>(&*Node))){
            return Error("failed to read page",Error::Layer::Pager);
        }
        path.push(std::move(Node));

        temp = &path.top();
    }

    bool duplicate = false;
    if(temp->key_number > 0){
        uint32_t idx = temp->Find_Key_Index(key);
        if(idx < temp->key_number && temp->keys[idx] == key){
            duplicate = true;
        }
    }

    if(temp->key_number < MAX_KEYS && !duplicate){
        if(temp->Insert_Value(key, value.c_str(), static_cast<uint32_t>(value.size()))){
            return true;
        }
    }

    if(duplicate){
        return Error("duplicate key",Error::Layer::Btree);
    }

    if(!Split_chain(*temp,pager,path,new_pages)){
        return Error("failed to split node",Error::Layer::Btree);
    }

    for(auto& np : new_pages){
        if(np->is_root()){
            out_root_page = np->page_num;
            break;
        }
    }

    /*split后，寻找正确的叶节点进行插入（key可能属于右半部分）*/
    Btree_Node* target = temp;
    if(target->next_leaf != 0){
        for(auto& np : new_pages){
            if(np->is_leaf() && np->page_num == target->next_leaf && np->key_number > 0){
                if(!(key < np->keys[0])){
                    target = np.get();
                }
                break;
            }
        }
    }

    if(!target->Insert_Value(key, value.c_str(), static_cast<uint32_t>(value.size()))){
        return Error("failed to insert value",Error::Layer::Btree);
    }

    return true;
}

/*
 * 更新：定位到叶子后原地改值。
 * update_page 返回被修改的节点副本，调用方直接用它落盘（不需要 StackPath）。
 */
Result<bool> Btree_update(uint32_t page_num,Key& key,std::string& value,Pager_t& pager,Btree_Node& update_page){
    auto Node = std::make_unique<Btree_Node>();
    Btree_Node* buffer = &*Node;
    if(!pager.read_page(page_num,reinterpret_cast<char*>(buffer))){
        return Error("failed to read page",Error::Layer::Pager);
    }

    while(buffer->type != Btree_Node::Type::LEAF_NODE){
        uint32_t next_page = buffer->Find_Next_Page(key);
        if(!pager.read_page(next_page,reinterpret_cast<char*>(buffer))){
            return Error("failed to read page",Error::Layer::Pager);
        }
    }

    if(!buffer->Update_Value(key, value.c_str(), static_cast<uint32_t>(value.size()))){
        return Error("failed to update value",Error::Layer::Btree);
    }

    update_page = *Node;
    return true;
}

/*
 * 删除主流程：
 *   叶子键数 > MAX_KEYS/2 → 直接删值即可，不会破环平衡
 *   否则先调用 Merge_chain 调整结构（借位或合并），再执行删除
 *
 * 注意顺序：先修复结构再删值，这样 Merge_chain 计算父节点的分隔键时
 * 叶子内容还是完整的。
 */
Result<bool> Btree_delete(uint32_t page_num,Key& key,Pager_t& pager,StackPath& path,std::vector<uint32_t>& del_pages,std::vector<std::unique_ptr<Btree_Node>>& update_pages,uint32_t& out_root_page){
    out_root_page = page_num;

    auto Node = std::make_unique<Btree_Node>();
    if(!pager.read_page(page_num,reinterpret_cast<char*>(&*Node))){
        return Error("failed to read page",Error::Layer::Pager);
    }
    path.push(std::move(Node));
    Btree_Node* temp = &path.top();

    while(temp->type != Btree_Node::Type::LEAF_NODE){
        uint32_t next_page = temp->Find_Next_Page(key);

        auto Node = std::make_unique<Btree_Node>();
        if(!pager.read_page(next_page,reinterpret_cast<char*>(&*Node))){
            return Error("failed to read page",Error::Layer::Pager);
        }
        path.push(std::move(Node));

        temp = &path.top();
    }

    if(temp->key_number > MAX_KEYS / 2){
        if(!temp->Delete_Value(key)){
            return Error("failed to delete value",Error::Layer::Btree);
        }
        return true;
    }

    if(!Merge_chain(*temp,pager,path,del_pages,update_pages,out_root_page)){
        return Error("failed to merge node",Error::Layer::Btree);
    }

    if(!temp->Delete_Value(key)){
        return Error("failed to delete value",Error::Layer::Btree);
    }

    return true;
}

/*
 * 清空整棵树（delete 无 where / drop table 时使用）。
 *
 * 用显式栈做迭代式后序遍历：
 *   flag == false 表示"还没展开子节点"，此时把所有子页压栈并把自身标记为已展开；
 *   flag == true  表示子节点都处理完了，可以处置自身：根节点清空内容保留页，
 *                 其余节点直接加入待删除列表。
 * 用显式栈而不是递归，是为了避免树较深时爆栈。
 */
Result<bool> Btree_delete_range(uint32_t page_num,Pager_t& pager,std::vector<uint32_t>& del_pages,uint32_t& out_root_page){
    out_root_page = page_num;
    auto root_node = std::make_unique<Btree_Node>();
    if (!pager.read_page(page_num, reinterpret_cast<char*>(&*root_node))) {
        return Error("failed to read page", Error::Layer::Pager);
    }

    using StackPair = std::pair<std::unique_ptr<Btree_Node>, bool>;
    std::stack<StackPair> record;

    record.push(std::make_pair(std::move(root_node), false));

    while(!record.empty()){
        auto& top_pair = record.top();
        Btree_Node& temp = *top_pair.first;
        bool& flag = top_pair.second;

        if(!temp.is_leaf() && !flag){
            flag = true;

            for(uint32_t i = 0; i <= temp.key_number; ++i){
                uint32_t child_pid = temp.son_pages[i];
                auto child_node = std::make_unique<Btree_Node>();
                if (!pager.read_page(child_pid, reinterpret_cast<char*>(&*child_node))){
                    return Error("failed to read page", Error::Layer::Pager);
                }
                record.push(std::make_pair(std::move(child_node), false));
            }
        }
        else {
            if(temp.is_root()){
                temp.clear();
                if(!pager.update_page(temp.page_num, reinterpret_cast<char*>(&temp))){
                    return Error("failed to write root", Error::Layer::Pager);
                }
            }
            else{
                del_pages.push_back(temp.page_num);
            }
            record.pop();
        }
    }
    return true;
}

/*
 * 分裂的级联传播：从发生分裂的节点开始，沿 StackPath 逐层向上。
 *
 * 每一轮：分裂当前节点 → 尝试把上浮键插入父节点
 *   父节点有空位 → 插入成功，结束
 *   父节点也满了 → 父节点成为下一轮分裂对象（pending_key / pending_page 暂存上浮键）
 *   当前节点是根   → 新建一个根，把两半作为它的两个子节点，树高加一
 *
 * pending_* 的作用：一次分裂可能产生两个上浮键（本轮新分裂的键，
 * 以及上一轮被推迟的键），需要判断它们分别插入父节点的键值范围。
 */
bool Split_chain(Btree_Node& curpage,Pager_t& pager,StackPath& path,std::vector<std::unique_ptr<Btree_Node>>& new_pages){
    Btree_Node* cur = &curpage;
    uint32_t pending_page = 0;
    Key pending_key;

    while(true){
        uint32_t new_page = pager.alloc_new_page();
        if(new_page == 0){
            return false;
        }
        auto new_node = std::make_unique<Btree_Node>();
        Key risekey;
        if(!cur->Page_Split(*new_node, new_page, risekey)){
            return false;
        }
        new_node->father = cur->father;
        new_pages.push_back(std::move(new_node));

        if(pending_page != 0){
            if(pending_key < risekey){
                cur->Insert_Key(pending_key, pending_page);
            } else {
                new_pages.back()->Insert_Key(pending_key, pending_page);
            }
        }

        if(cur->is_root()){
            uint32_t new_root_page = pager.alloc_new_page();
            if(new_root_page == 0){
                return false;
            }
            auto new_root = std::make_unique<Btree_Node>();
            create_new_root(cur->page_num, new_page, risekey, *new_root, new_root_page);
            cur->root = false;
            cur->father = new_root_page;
            new_pages.back()->father = new_root_page;
            new_pages.push_back(std::move(new_root));
            return true;
        }

        path.pop();
        Btree_Node& parent = path.top();

        if(parent.key_number < MAX_KEYS){
            parent.Insert_Key(risekey, new_page);
            return true;
        }

        pending_page = new_page;
        pending_key = risekey;
        cur = &parent;
    }
}

/*
 * 借位/合并的级联传播：从欠载的节点开始，沿 StackPath 逐层向上。
 *
 * 每层的处置优先级：
 *   1. 向左右兄弟借一个键（兄弟有余量时）—— 代价最小，处理完即可结束
 *   2. 借不到则与兄弟合并 —— 父节点少一个键，可能自己也欠载，继续向上
 *   3. 若合并的是叶子，还要修正相邻叶子的 pre/next 指针，保持链表完整
 *
 * 根的特殊处理：父节点键数归零时，唯一子节点升为新根，树高减一。
 *
 * update_pages / del_pages 分别收集"需要回写"和"需要释放"的页，
 * 由调用方统一落盘，避免在级联过程中反复 IO。
 */
bool Merge_chain(Btree_Node& curpage,Pager_t& pager,StackPath& path,std::vector<uint32_t>& del_pages,std::vector<std::unique_ptr<Btree_Node>>& update_pages,uint32_t& out_root_page){/*uodatepage/delpage只存左右变动结点*/
    Btree_Node* cur = &curpage;

    while (true){

        if(cur->key_number > MAX_KEYS / 2){
            break;
        }

        path.pop();
        if (path.empty()){
            break;
        }

        Btree_Node& parent = path.top();
        uint32_t parent_pid = parent.page_num;

        Key prekey = cur->keys[0];
        uint32_t idx = parent.Find_Key_Index(prekey);
        uint32_t cur_index;
        if (idx < parent.key_number && parent.keys[idx] == prekey) {
            cur_index = idx + 1;
        } else {
            cur_index = idx;
        }

        uint32_t left_pid = (cur_index > 0) ? parent.son_pages[cur_index - 1] : 0;
        uint32_t right_pid = (cur_index < parent.key_number) ? parent.son_pages[cur_index + 1] : 0;

        Key separator_key;
        if (cur_index > 0) {
            separator_key = parent.keys[cur_index - 1];
        }

        bool          borrow_success = false;
        bool          merge_success  = false;
        Key           del_key;
        uint32_t      recycle_pid    = 0;
        std::unique_ptr<Btree_Node> sibling_ptr = nullptr;

        if(left_pid != 0){
            sibling_ptr = std::make_unique<Btree_Node>();
            if(!pager.read_page(left_pid, reinterpret_cast<char*>(sibling_ptr.get()))){
                return false;
            }
            if(cur->Borrow_Left(*sibling_ptr)){
                borrow_success = true;
                parent.Update_Key(separator_key, cur->keys[0]);
                update_pages.push_back(std::move(sibling_ptr));
            }
        }

        if(!borrow_success && right_pid != 0){
            sibling_ptr = std::make_unique<Btree_Node>();
            if(!pager.read_page(right_pid, reinterpret_cast<char*>(sibling_ptr.get()))){
                return false;
            }
            Key right_first_key = sibling_ptr->keys[0];
            if(cur->Borrow_Right(*sibling_ptr)){
                borrow_success = true;
                parent.Update_Key(right_first_key, sibling_ptr->keys[0]);
                update_pages.push_back(std::move(sibling_ptr));
            }
        }

        if(!borrow_success){
            if(left_pid != 0){
                sibling_ptr = std::make_unique<Btree_Node>();
                if(!pager.read_page(left_pid, reinterpret_cast<char*>(sibling_ptr.get()))){
                    return false;
                }
                if(cur->Merge_Left(*sibling_ptr, del_key)){
                    merge_success = true;
                    recycle_pid = left_pid;
                    del_pages.push_back(left_pid);
                } else {
                    return false;
                }
            } else if(right_pid != 0){
                sibling_ptr = std::make_unique<Btree_Node>();
                if(!pager.read_page(right_pid, reinterpret_cast<char*>(sibling_ptr.get()))){
                    return false;
                }
                if(cur->Merge_Right(*sibling_ptr, del_key)){
                    merge_success = true;
                    recycle_pid = right_pid;
                    del_pages.push_back(right_pid);
                } else {
                    return false;
                }
            } else {
                return false;
            }

            if(merge_success && recycle_pid != 0){
                parent.Delete_Key(del_key, recycle_pid);

                if(cur->is_leaf()){
                    if(left_pid == recycle_pid){
                        uint32_t prev_pid = cur->pre_leaf;
                        if(prev_pid != 0){
                            auto prev_node = std::make_unique<Btree_Node>();
                            if(pager.read_page(prev_pid, reinterpret_cast<char*>(prev_node.get()))){
                                prev_node->next_leaf = cur->page_num;
                                prev_node->dirty = true;
                                update_pages.push_back(std::move(prev_node));
                            }
                        }
                    } else {
                        uint32_t next_pid = cur->next_leaf;
                        if(next_pid != 0){
                            auto next_node = std::make_unique<Btree_Node>();
                            if(pager.read_page(next_pid, reinterpret_cast<char*>(next_node.get()))){
                                next_node->pre_leaf = cur->page_num;
                                next_node->dirty = true;
                                update_pages.push_back(std::move(next_node));
                            }
                        }
                    }
                }
            }
        }

        if(path.size() == 1){
            if (!parent.is_leaf() && parent.key_number == 0){
                del_pages.push_back(parent_pid);
                cur->root = true;
                cur->dirty = true;
                out_root_page = cur->page_num;
                break;
            }
        }

        if(borrow_success){
            break;
        }

        cur = &parent;
    }

    return true;
}


/* 根分裂时构造新根：只有一个分隔键，两个子指针分别指向分裂出的两半 */
bool create_new_root(uint32_t page1,uint32_t page2,Key& risekey,Btree_Node& new_root,uint32_t new_root_page){
    new_root.dirty = true;
    new_root.page_num = new_root_page;
    new_root.type = Btree_Node::Type::MID_NODE;
    new_root.key_number = 1;
    new_root.root = true;

    new_root.keys[0] = risekey;
    new_root.son_pages[0] = page1;
    new_root.son_pages[1] = page2;
    return true;
}

/*
 * 调试区：dump 节点内容 + 校验节点级不变量。
 * validate() 只检查单个节点自身的性质，不跨节点检查父子关系。
 */
#ifdef DEBUG

void Btree_Node::print(FILE* fp) const {
    fprintf(fp, "=== Node page=%u type=%s key_num=%u dirty=%d root=%d father=%u\n",
            page_num, is_leaf() ? "LEAF" : "MID", key_number, dirty, root, father);
    fprintf(fp, "keys: ");
    for (uint32_t i = 0; i < key_number; i++) { // 先只打前8个键，避免太长
        if (keys[i].type == Key::Type::KEY_INT)
            fprintf(fp, "%d ", keys[i].val_int);
        else if (keys[i].type == Key::Type::KEY_VARCHAR)
            fprintf(fp, "\"%.*s\" ", (int)keys[i].length, keys[i].str);
    }
    fprintf(fp, "\n");
    if (is_leaf()) {
        fprintf(fp, "curval=%u next_leaf=%u pre_leaf=%u\n", curval, next_leaf, pre_leaf);
        for(uint32_t i = 0;i < key_number;i++){
            std::cout << "offsets[" << i << "] = " << offsets[i] << ' ';
        }
        std::cout << '\n';
        std::cout << "curval = " << curval << std::endl;
        std::cout << "values:" << values << std::endl;
    } else {
        fprintf(fp, "first_son=%u\n", son_pages[0]);
    }
    fflush(fp);
}

/* 校验单节点不变量：键数上限、offsets 单调、键有序、values 容量 */
bool Btree_Node::validate() const {
    if (key_number > MAX_KEYS) return false;
    if (is_leaf()) {
        if (key_number > 0 && offsets[0] != 0) return false;
        for (uint32_t i = 1; i < key_number; i++) {
            if (offsets[i] <= offsets[i-1]) return false;
            if (!(keys[i-1] < keys[i])) return false;
        }
        if (curval > PAGE_SIZE - HEAD_SIZE) return false;
    } else {
        for (uint32_t i = 1; i < key_number; i++) {
            if (!(keys[i-1] < keys[i])) return false;
        }
    }
    return true;
}

#endif