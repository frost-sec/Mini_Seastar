#include "Packet.h"
#include <iostream>
#include <cassert>

int main() {
    // 1. 创建原始包
    std::cout << "--- Test 1: Share ---" << std::endl;
    Packet p1 = Packet::from_string("Hello World");
    std::cout << "p1: " << p1.to_string() << " (refs: " << p1.use_count() << ")" << std::endl;

    // 2. 广播 (Share)
    {
        Packet p2 = p1.share();
        std::cout << "p2: " << p2.to_string() << " (refs: " << p1.use_count() << ")" << std::endl;
        assert(p1.use_count() == 2);
        
        // 修改 p2，p1 应该也变 (因为是同一块内存)
        p2.data()[0] = 'h'; 
    } // p2 析构，引用计数 -1

    std::cout << "After p2 dies: " << p1.to_string() << " (refs: " << p1.use_count() << ")" << std::endl;
    assert(p1.use_count() == 1);
    assert(p1.to_string() == "hello World"); // 验证修改生效

    // 3. 切片 (Slice)
    std::cout << "\n--- Test 2: Slice ---" << std::endl;
    Packet full = Packet::from_string("[Header]Payload");
    
    // 模拟剥离 8 字节头部
    Packet payload = full.slice(8, 7); 
    
    std::cout << "Full: " << full.to_string() << std::endl;
    std::cout << "Payload: " << payload.to_string() << std::endl;
    
    // 验证是零拷贝
    assert(payload.to_string() == "Payload");
    assert((void*)payload.data() == (void*)(full.data() + 8)); // 指针算术验证

    std::cout << "✅ All Zero-Copy tests passed!" << std::endl;

    return 0;
}