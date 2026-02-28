#pragma once
#include <vector>
#include <unordered_map>
#include <deque>
#include <functional>
#include <queue>
#include <memory>
#include <mutex>
#include <chrono>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <thread>
#include "Future.h"
#include "SpscQueue.h"

using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

struct TimerTask {
    TimePoint expire_time;
    std::function<void()> callback;
    uint64_t id;
    bool operator>(const TimerTask& other) const {
        return expire_time > other.expire_time;
    }
};

template<typename T> class Future;
template<typename T> class Promise;

// handler 接收事件掩码，用于区分 EPOLLIN / EPOLLOUT
using EventHandler = std::function<void(uint32_t events)>;

class Reactor {
private:
    int epoll_fd;
    int notify_fd;
    int timer_fd;

    std::priority_queue<TimerTask, std::vector<TimerTask>,
                        std::greater<TimerTask>> timers_;

    std::unordered_map<int, EventHandler> handlers;
    std::deque<std::function<void()>> pending_tasks;

    std::vector<<std::function<void()>,1024>> cross_core_queue_;

    static thread_local Reactor* instance_;

public:
    Reactor();
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    static Reactor* instance() { return instance_; }

    // 注册 fd，自动附加 EPOLLET
    void add(int fd, uint32_t events, EventHandler handler);

    // 只修改事件掩码，不换 handler（用于开关 EPOLLOUT）
    void modify_events(int fd, uint32_t events);

    // 从 epoll 中移除 fd
    void remove(int fd);

    void schedule(std::function<void()> task);
    void submit_task(std::function<void()> task);
    void run();

    void run_at(TimePoint timestamp, std::function<void()> callback);
    void run_after(int delay_ms, std::function<void()> callback);
    Future<void> sleep(int seconds);

private:
    void handle_incoming_tasks();
    void reset_timer_fd();
    void handle_timer_events();
};