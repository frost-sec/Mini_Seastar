#pragma once
#include <vector>
#include <map>
#include <deque>
#include <functional>
#include <queue>
#include <memory>
#include <mutex>
#include <chrono>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include "Future.h"

using Clock=std::chrono::steady_clock;
using TimePoint=std::chrono::time_point<Clock>;

struct TimerTask{
    TimePoint expire_time;
    std::function<void()> callback;
    uint64_t id;

    bool operator>(const TimerTask& other) const{
        return expire_time>other.expire_time;
    }
};

template<typename T> class Future;
template<typename T> class Promise;

class Reactor {
private:
    int epoll_fd;
    int notify_fd;
    int timer_fd;

    std::priority_queue<TimerTask,std::vector<TimerTask>,std::greater<TimerTask>> timers_;

    std::map<int, std::function<void()>> handlers;
    std::deque<std::function<void()>> pending_tasks;

    std::vector<std::function<void()>> incoming_tasks;
    std::mutex task_mutex;

    static thread_local Reactor* instance_; 

public:
    Reactor();
    ~Reactor();
    
    // 禁用拷贝
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    // 获取单例（或者全局实例）的辅助函数
    static Reactor* instance() { return instance_; }

    void add(int fd, std::function<void()> handler);
    void add(int fd,uint32_t events,std::function<void()> handler);
    void remove(int fd);
    void schedule(std::function<void()> task);

    void submit_task(std::function<void()> task);

    void run();

    void run_at(TimePoint timestamp,std::function<void()> callback);
    void run_after(int delay_ms,std::function<void()> callback);
    
    Future<void> sleep(int seconds);

private: 
    void handle_incoming_tasks();
    void reset_timer_fd();
    void handle_timer_events();
};