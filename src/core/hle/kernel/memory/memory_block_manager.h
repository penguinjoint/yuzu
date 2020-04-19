// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <list>
#include <memory>

#include "common/common_types.h"
#include "core/hle/kernel/memory/memory_block.h"

namespace Kernel::Memory {

class MemoryBlockManager final {
public:
    using MemoryBlockTree = std::list<MemoryBlock>;
    using iterator = MemoryBlockTree::iterator;
    using const_iterator = MemoryBlockTree::const_iterator;

public:
    MemoryBlockManager(VAddr start_addr, VAddr end_addr);

    iterator end() {
        return memory_block_tree.end();
    }
    const_iterator end() const {
        return memory_block_tree.end();
    }
    const_iterator cend() const {
        return memory_block_tree.cend();
    }

    iterator FindIterator(VAddr addr);

    VAddr FindFreeArea(VAddr region_start, std::size_t region_num_pages, std::size_t num_pages,
                       std::size_t align, std::size_t offset, std::size_t guard_pages);

    void Update(VAddr addr, std::size_t num_pages, MemoryState prev_state,
                MemoryPermission prev_perm, MemoryAttribute prev_attribute, MemoryState state,
                MemoryPermission perm, MemoryAttribute attribute);

    void Update(VAddr addr, std::size_t num_pages, MemoryState state,
                MemoryPermission perm = MemoryPermission::None,
                MemoryAttribute attribute = MemoryAttribute::None);

    using IterateFunc = std::function<void(const MemoryInfo&)>;
    void IterateForRange(VAddr start, VAddr end, IterateFunc&& func);

    MemoryBlock& FindBlock(VAddr addr) {
        return *FindIterator(addr);
    }

private:
    void MergeAdjacent(iterator it, iterator& next_it);

    const VAddr start_addr;
    const VAddr end_addr;

    MemoryBlockTree memory_block_tree;
};

} // namespace Kernel::Memory
