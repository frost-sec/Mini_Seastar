#pragma once

#include <vector>
#include <thread>
#include <functional>
#include <atomic>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <pthread.h>
#include <sched.h>
#include "Reactor.h"

namespace seastar{
    inline std::vector<Reactor*> g_reactors;
    inline thread_local int g_cpu_id=-1;

    inline int cpu_id(){
        return g_cpu_id;
    }

    class Engine{
    private:
        std::vector<std::thread> threads_;
        std::atomic<int> ready_count_{0};

        int num_cpus_;

    public:
        Engine(){
            num_cpus_=std::thread::hardware_concurrency();
            g_reactors.resize(num_cpus_);
        }
        ~Engine(){
            stop();
        }
        
        template <typename Func>
        void run(Func&& user_main){
            for(int i=0;i<num_cpus_;++i){
                threads_.emplace_back([this,i,user_main](){
                    g_cpu_id=i;

                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    CPU_SET(i,&cpuset);

                    int rc=pthread_setaffinity_np(pthread_self(),sizeof(cpu_set_t),&cpuset);
                    if(rc!=0){
                        std::cerr<<"Error calling pthread_setaffinity_np on core"<<i<<std::endl;
                    }

                    Reactor reactor;
                    g_reactors[i]=&reactor;

                    ready_count_++;
                    while(ready_count_<num_cpus_){
                        std::this_thread::yield();
                    }

                    user_main();
                    reactor.run();
                });
            }

            for(auto& t:threads_){
                if(t.joinable()) t.join();
            }
        }

        void stop(){
            // 这里未来可以实现通知所有 Reactor 退出循环
        // 目前先留空
        }

        static void submit_to(int cpu_id,std::function<void()> task){
            if(cpu_id<0 || cpu_id>=g_reactors.size()) return;
            g_reactors[cpu_id]->submit_task(std::move(task));
        }
    };
}