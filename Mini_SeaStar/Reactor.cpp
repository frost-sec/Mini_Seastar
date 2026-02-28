#include "Reactor.h"
#include "Future.h" 
#include <stdexcept>
#include <iostream>
#include <unistd.h>
#include <sys/timerfd.h>
#include <cstring> 

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
    if(epoll_fd < 0) throw std::runtime_error("Failure to create epoll");

    notify_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    if(notify_fd<0) throw std::runtime_error("Failure to create eventfd");

    struct epoll_event ev;
    ev.events=EPOLLIN;
    ev.data.fd=notify_fd;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,notify_fd,&ev)!=0){
        throw std::runtime_error("Failed to add notify_fd to epoll");
    }

    timer_fd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    if(timer_fd<0) throw std::runtime_error("Failed to create timerfd");
    ev.data.fd=timer_fd;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,timer_fd,&ev)!=0){
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

void Reactor::add(int fd, std::function<void()> handler) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    handlers[fd] = handler;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0)
        throw std::runtime_error("Register failed: " + std::to_string(fd));
}

void Reactor::add(int fd, uint32_t events, std::function<void()> handler) {
    struct epoll_event ev;
    ev.events = events; // ✅ 使用传入的事件 (EPOLLIN 或 EPOLLOUT)
    ev.data.fd = fd;
    handlers[fd] = std::move(handler);
    
    // 注意：这里需要判断是 EPOLL_CTL_ADD 还是 MOD
    // 简单起见，我们假设 fd 每次处理完都会 remove，所以总是 ADD
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0){
        return;
    }

    if(errno==EEXIST){
        if(epoll_ctl(epoll_fd,EPOLL_CTL_MOD,fd,&ev)==0){
            return;
        }
    }

    perror("epoll_ctl failed");
    throw std::runtime_error("Ractor::add failed for fd "+std::to_string(fd));
}

void Reactor::remove(int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    handlers.erase(fd);
}

void Reactor::schedule(std::function<void()> task) {
    pending_tasks.push_back(std::move(task));
}

void Reactor::run_after(int delay_ms,std::function<void()> callback){
    auto expire_time=Clock::now()+std::chrono::milliseconds(delay_ms);
    run_at(expire_time,std::move(callback));
}

void Reactor::run_at(TimePoint timestamp,std::function<void()> callback){
    bool earliest_changed=false;

    if(timers_.empty() || timestamp<timers_.top().expire_time){
        earliest_changed=true;
    }

    timers_.push({timestamp,std::move(callback),0});

    if(earliest_changed){
        reset_timer_fd();
    }
}

void Reactor::reset_timer_fd(){
    if(timers_.empty()) return;

    TimePoint next_expire=timers_.top().expire_time;

    auto now=Clock::now();
    int64_t diff_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(next_expire-now).count();
    if(diff_ns<100) diff_ns=100;

    struct itimerspec new_value;
    new_value.it_value.tv_sec = diff_ns / 1000000000;
    new_value.it_value.tv_nsec = diff_ns % 1000000000;
    new_value.it_interval.tv_sec = 0; // 一次性的，不是周期性的
    new_value.it_interval.tv_nsec = 0;

    timerfd_settime(timer_fd, 0, &new_value, nullptr);
}

void Reactor::handle_timer_events(){
    uint64_t exp;
    ::read(timer_fd,&exp,sizeof(uint64_t));

    auto now=Clock::now();
    while(!timers_.empty() && timers_.top().expire_time<=now){
        TimerTask task=timers_.top();
        timers_.pop();

        if(task.callback) task.callback();
    }

    if(!timers_.empty()){
        reset_timer_fd();
    }
}

Future<void> Reactor::sleep(int seconds) {
    auto promise = std::make_shared<Promise<void>>();
    run_after(seconds * 1000, [promise](){
        promise->set_value();
    });
    return promise->get_future();
}

void Reactor::submit_task(std::function<void()> task){
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        incoming_tasks.push_back(std::move(task));
    }

    uint64_t u=1;
    ssize_t s=::write(notify_fd,&u,sizeof(uint64_t));

    if(s!=sizeof(uint64_t)){
        //错误处理？
    }
}

void Reactor::run() {
    const int MAX_EVENTS = 10;
    struct epoll_event events[MAX_EVENTS];
    std::cout << "Reactor loop starting..." << std::endl;

    while(true) {
        while(!pending_tasks.empty()) {
            auto task = std::move(pending_tasks.front());
            pending_tasks.pop_front();
            task();
        }
        
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for(int i=0; i<n; ++i) {
            int active_fd=events[i].data.fd;
            if(active_fd==notify_fd){
                uint64_t u;
                ::read(notify_fd,&u,sizeof(uint64_t));
                handle_incoming_tasks();
            }else if(active_fd==timer_fd){
                handle_timer_events();
            }
            else{
                auto it = handlers.find(events[i].data.fd);
                if(it != handlers.end()){
                    auto handler=it->second;
                    handler();
                }   
            }
            
        }
    }
}

void Reactor::handle_incoming_tasks(){
    std::vector<std::function<void()>> temp_tasks;
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        temp_tasks.swap(incoming_tasks);
    }

    for(auto& task:temp_tasks){
        task();
    }
}