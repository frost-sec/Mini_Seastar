#pragma once
#include <vector>
#include <cassert>
#include <cstddef>
#include <thread>
#include <new>

template <typename T,size_t ChunkSize=256>
class Poolable
{
public:
    static void* operator new(size_t size){
        assert(size==sizeof(T));
        return get_pool().allocate();
    }
    static void operator delete(void* ptr){
        if(!ptr) return;
        get_pool().deallocate(ptr);
    }

private:
    //1.为什么用union不用variant：
    //variant内有一个tag去记录用的是那种类型，会带来额外开销
    //同时其是强类型会自动管理对象生命周期，其会触发大量类型检查和内部构造逻辑
    //2.复用内存：C11中有专门的std::aligned_storage和std::aligned_union
    //但是C23已经正式废弃，委员会推荐最佳实践就是union加alignas对齐
    union Node
    {
        Node* next;
        alignas(alignof(T)) unsigned char storage[sizeof(T)];
    };

    class ThreadLocalPool{
    private:
        Node* head_=nullptr;
        std::vector<Node*> chunks_;
        const std::thread::id thread_id_;

        void allocate_chunk(){
            //一次性分配ChunkSize节点连续内存
            Node* chunk=static_cast<Node*>(::operator new(sizeof(Node)*ChunkSize));
            chunks_.push_back(chunk);

            //大内存切分多个Node，串联成侵入式链表
            //LIFO的Cache局部性，从后往前串联
            for(size_t i=0;i<ChunkSize-1;++i){
                chunk[i].next=&chunk[i+1];
            }
            chunk[ChunkSize-1].next=head_;
            head_=chunk;
        }
        
    public:
        ThreadLocalPool():thread_id_(std::this_thread::get_id()){
            allocate_chunk();
        }
        ~ThreadLocalPool(){
            for(Node* chunk:chunks_){
                ::operator delete(chunk);
            }
        }

        void* allocate(){
            assert(std::this_thread::get_id()==thread_id_ && "Strict Shared-Nothing: Cross-thread allocation forbidden!");
            if(head_==nullptr){
                allocate_chunk();
            }

            Node* node=head_;
            head_=head_->next;
            return node;
        }

        void deallocate(void* ptr){
            assert(std::this_thread::get_id()==thread_id_ && "Strict Shared-Nothing: Cross-thread allocation forbidden!");

            Node* node=static_cast<Node*>(ptr);
            node->next=head_;
            head_=node;
        }
    };

    static ThreadLocalPool& get_pool(){
        static thread_local ThreadLocalPool pool;
        return pool;
    }
    
};