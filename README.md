# Mini_Seastar
A Seastar-inspired Shared-Nothing async network framework. Iteratively optimized for high performance: v1 (Level-Triggered); v2 (Edge-Triggered + Slab Allocator); v3 (custom Intrusive Pointers replacing std::shared_ptr to eliminate atomic overhead). Designed for exploring system principles and performance bottlenecks.
