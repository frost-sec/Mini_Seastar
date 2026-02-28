#include "Reactor.h"
#include "Future.h"
#include <stdexcept>
#include <iostream>
#include <unistd.h>
#include <sys/timerfd.h>
#include <cstring>
#include <thread>

void schedule_task(std::function<void()> task) {
    if (Reactor::instance()) {
        Reactor::instance()->schedule(std::move(task));
    } else {
        std::cerr << "Error: Reactor not initialized!" << std::endl;
    }
}

thread_local Reactor* Reactor::instance_ = nullptr;

Reactor::Reactor() {
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) throw std::runtime_error("Failure to create epoll");

    notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (notify_fd < 0) throw std::runtime_error("Failure to create eventfd");

    // notify_fd 和 timer_fd 仍用 LT 模式（内部 fd，不走 handlers）
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = notify_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, notify_fd, &ev) != 0) {
        throw std::runtime_error("Failed to add notify_fd to epoll");
    }

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) throw std::runtime_error("Failed to create timerfd");
    ev.data.fd = timer_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) != 0) {
        throw std::runtime_error("Fail to add timer_fd to epoll");
    }

    if (instance_ != nullptr) throw std::runtime_error("Reactor already exists!");
    instance_ = this;
}

Reactor::~Reactor() {
    close(notify_fd);
    close(epoll_fd);
    close(timer_fd);
    instance_ = nullptr;
}

// 注册 fd 到 epoll，强制附加 EPOLLET
void Reactor::add(int fd, uint32_t events, EventHandler handler) {
    struct epoll_event ev;
    ev.events = events | EPOLLET;  // 强制 ET 模式
    ev.data.fd = fd;
    handlers[fd] = std::move(handler);

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
        return;
    }
    if (errno == EEXIST) {
        // 已存在则 MOD（防御性编程）
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
            return;
        }
    }
    perror("epoll_ctl ADD failed");
    throw std::runtime_error("Reactor::add failed for fd " + std::to_string(fd));
}

// 只修改事件掩码，handler 不变
void Reactor::modify_events(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events | EPOLLET;  // 始终保持 ET
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) {
        perror("epoll_ctl MOD failed");
    }
}

void Reactor::remove(int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    handlers.erase(fd);
}

void Reactor::schedule(std::function<void()> task) {
    pending_tasks.push_back(std::move(task));
}

void Reactor::run_after(int delay_ms, std::function<void()> callback) {
    auto expire_time = Clock::now() + std::chrono::milliseconds(delay_ms);
    run_at(expire_time, std::move(callback));
}

void Reactor::run_at(TimePoint timestamp, std::function<void()> callback) {
    bool earliest_changed = false;
    if (timers_.empty() || timestamp < timers_.top().expire_time) {
        earliest_changed = true;
    }
    timers_.push({timestamp, std::move(callback), 0});
    if (earliest_changed) {
        reset_timer_fd();
    }
}

void Reactor::reset_timer_fd() {
    if (timers_.empty()) return;

    TimePoint next_expire = timers_.top().expire_time;
    auto now = Clock::now();
    int64_t diff_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        next_expire - now).count();
    if (diff_ns < 100) diff_ns = 100;

    struct itimerspec new_value;
    new_value.it_value.tv_sec  = diff_ns / 1000000000;
    new_value.it_value.tv_nsec = diff_ns % 1000000000;
    new_value.it_interval.tv_sec  = 0;
    new_value.it_interval.tv_nsec = 0;
    timerfd_settime(timer_fd, 0, &new_value, nullptr);
}

void Reactor::handle_timer_events() {
    uint64_t exp;
    ::read(timer_fd, &exp, sizeof(uint64_t));

    auto now = Clock::now();
    while (!timers_.empty() && timers_.top().expire_time <= now) {
        TimerTask task = timers_.top();
        timers_.pop();
        if (task.callback) task.callback();
    }
    if (!timers_.empty()) {
        reset_timer_fd();
    }
}

Future<void> Reactor::sleep(int seconds) {
    auto promise = std::make_shared<Promise<void>>();
    run_after(seconds * 1000, [promise]() {
        promise->set_value();
    });
    return promise->get_future();
}

void Reactor::submit_task(std::function<void()> task) {
    while(!cross_core_queue_.push(std::move(task))){
        std::this_thread::yield();
    }
    uint64_t u = 1;
    ::write(notify_fd, &u, sizeof(uint64_t));
}

void Reactor::run() {
    const int MAX_EVENTS =128;  // 增大批量处理能力
    struct epoll_event events[MAX_EVENTS];
    std::cout << "Reactor loop starting..." << std::endl;

    while (true) {
        // 先处理 pending tasks
        while (!pending_tasks.empty()) {
            auto task = std::move(pending_tasks.front());
            pending_tasks.pop_front();
            task();
        }

        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == notify_fd) {
                uint64_t u;
                ::read(notify_fd, &u, sizeof(u));
                handle_incoming_tasks();
            } else if (fd == timer_fd) {
                handle_timer_events();
            } else {
                // 传递事件掩码给 handler
                auto it = handlers.find(fd);
                if (it != handlers.end()) {
                    it->second(ev);
                }
            }
        }
    }
}

void Reactor::handle_incoming_tasks() {
    std::function<void()> task;
    while(cross_core_queue_.pop(task)){
        if(task){
            task();
        }
    }
}