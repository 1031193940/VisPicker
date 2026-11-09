#pragma once

#include <windows.h>
#include <psapi.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <new>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace vispick::test {

// Optional soak diagnostic. Capture writes JSONL and compares each complete snapshot
// with the preceding complete snapshot. Stage names are fixed printable ASCII labels.
class SoakMemoryProbe {
public:
    SoakMemoryProbe() {
        void* memory = VirtualAlloc(nullptr, sizeof(Workspace), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!memory) { throw std::runtime_error("Allocate soak memory probe workspace failed"); }
        workspace_ = new (memory) Workspace{};
    }

    ~SoakMemoryProbe() { VirtualFree(workspace_, 0, MEM_RELEASE); }
    SoakMemoryProbe(const SoakMemoryProbe&) = delete;
    SoakMemoryProbe& operator=(const SoakMemoryProbe&) = delete;

    void Capture(std::ostream& output, unsigned cycle, std::string_view stage) {
        Snapshot& current = workspace_->snapshots[nextSnapshot_];
        current.count = 0;
        const auto countersBefore = PrivateUsage();
        const auto defaultHeap = GetProcessHeap();
        HeapTotals heap{};
        const DWORD heapError = ReadHeap(defaultHeap, heap);
        Totals totals{};
        const DWORD error = ReadSnapshot(current, totals);
        const auto countersAfter = PrivateUsage();
        output << "{\"cycle\":" << cycle << ",\"stage\":" << std::quoted(stage)
               << ",\"private_usage_before\":" << countersBefore
               << ",\"private_usage_after\":" << countersAfter;
        if (error) {
            output << ",\"error\":" << error << "}\n";
            return;
        }
        output << ",\"mem_private_commit\":" << totals.privateCommit
               << ",\"mapped_commit\":" << totals.mappedCommit
               << ",\"image_commit\":" << totals.imageCommit
               << ",\"sampler_commit\":" << totals.samplerCommit
               << ",\"heap_base_commit\":" << totals.heapBaseCommit
               << ",\"stack_commit\":" << totals.stackCommit
               << ",\"stack_candidate_commit\":" << totals.stackCandidateCommit
               << ",\"other_private_commit\":" << totals.otherPrivateCommit
               << ",\"heap_count\":" << totals.heapCount
               << ",\"default_heap_base\":" << reinterpret_cast<std::uintptr_t>(defaultHeap)
               << ",\"default_heap_walk_error\":" << heapError;
        if (!heapError) {
            output << ",\"default_heap_busy\":" << heap.busyBytes
                   << ",\"default_heap_busy_blocks\":" << heap.busyBlocks
                   << ",\"default_heap_free\":" << heap.freeBytes
                   << ",\"default_heap_region_commit\":" << heap.regionCommit
                   << ",\"default_heap_regions_without_commit\":" << heap.regionsWithoutCommit;
        }
        output
               << ",\"private_allocations\":" << current.count
               << ",\"changes\":[";
        if (hasPrevious_) { WriteChanges(output, workspace_->snapshots[1 - nextSnapshot_], current); }
        output << "]}\n";
        nextSnapshot_ = 1 - nextSnapshot_;
        hasPrevious_ = true;
    }

private:
    enum class Kind { Other, HeapBase, Stack, StackCandidate };

    struct Allocation {
        std::uintptr_t base = 0;
        SIZE_T committed = 0;
        SIZE_T reserved = 0;
        Kind kind = Kind::Other;
    };

    struct Snapshot {
        std::array<Allocation, 1024> allocations;
        std::size_t count = 0;
    };

    struct Workspace {
        std::array<Snapshot, 2> snapshots;
        std::array<HANDLE, 256> heaps;
    };

    struct Totals {
        SIZE_T privateCommit = 0;
        SIZE_T mappedCommit = 0;
        SIZE_T imageCommit = 0;
        SIZE_T samplerCommit = 0;
        SIZE_T heapBaseCommit = 0;
        SIZE_T stackCommit = 0;
        SIZE_T stackCandidateCommit = 0;
        SIZE_T otherPrivateCommit = 0;
        DWORD heapCount = 0;
    };

    struct HeapTotals {
        SIZE_T busyBytes = 0;
        SIZE_T busyBlocks = 0;
        SIZE_T freeBytes = 0;
        SIZE_T regionCommit = 0;
        SIZE_T regionsWithoutCommit = 0;
    };

    static SIZE_T PrivateUsage() noexcept {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                    sizeof(counters)) ? counters.PrivateUsage : 0;
    }

    static DWORD ReadHeap(HANDLE heap, HeapTotals& totals) noexcept {
        // Only walk the default heap. A DLL may configure or destroy its own private heap.
        if (!heap) { return GetLastError(); }
        if (!HeapLock(heap)) { return GetLastError(); }
        DWORD error = ERROR_MORE_DATA;
        PROCESS_HEAP_ENTRY entry{};
        constexpr std::size_t maximumEntries = 65536;
        for (std::size_t entryIndex = 0; entryIndex < maximumEntries; ++entryIndex) {
            if (!HeapWalk(heap, &entry)) {
                error = GetLastError();
                if (error == ERROR_NO_MORE_ITEMS) { error = ERROR_SUCCESS; }
                break;
            }
            if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                totals.busyBytes += entry.cbData;
                ++totals.busyBlocks;
            } else if (entry.wFlags & PROCESS_HEAP_REGION) {
                totals.regionCommit += entry.Region.dwCommittedSize;
                totals.regionsWithoutCommit += entry.Region.dwCommittedSize == 0;
            } else if (entry.wFlags == 0) {
                totals.freeBytes += entry.cbData;
            }
        }
        if (!HeapUnlock(heap)) { return GetLastError(); }
        return error;
    }

    DWORD ReadSnapshot(Snapshot& snapshot, Totals& totals) noexcept {
        totals.heapCount = GetProcessHeaps(static_cast<DWORD>(workspace_->heaps.size()), workspace_->heaps.data());
        if (!totals.heapCount) { return GetLastError(); }
        if (totals.heapCount > workspace_->heaps.size()) { return ERROR_INSUFFICIENT_BUFFER; }

        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(&region, &region, sizeof(region))) { return GetLastError(); }
        const auto stackBase = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
        const auto samplerBase = reinterpret_cast<std::uintptr_t>(workspace_);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto lastAddress = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
        auto address = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
        Allocation allocation{};
        bool hasGuard = false;
        while (address <= lastAddress) {
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region))) { return GetLastError(); }
            const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (next <= address) { return ERROR_INVALID_DATA; }
            address = next;

            const auto base = region.State == MEM_FREE ? 0 : reinterpret_cast<std::uintptr_t>(region.AllocationBase);
            if (base != allocation.base) {
                if (!Store(snapshot, totals, allocation, stackBase, hasGuard)) { return ERROR_INSUFFICIENT_BUFFER; }
                allocation = {};
                hasGuard = false;
            }
            if (region.State == MEM_FREE) { continue; }

            if (region.State == MEM_COMMIT) {
                if (region.Type == MEM_MAPPED) { totals.mappedCommit += region.RegionSize; }
                if (region.Type == MEM_IMAGE) { totals.imageCommit += region.RegionSize; }
            }
            if (region.Type != MEM_PRIVATE) { continue; }
            if (base == samplerBase) {
                if (region.State == MEM_COMMIT) { totals.samplerCommit += region.RegionSize; }
                continue;
            }
            allocation.base = base;
            if (region.State == MEM_RESERVE) { allocation.reserved += region.RegionSize; }
            if (region.State == MEM_COMMIT) {
                allocation.committed += region.RegionSize;
                hasGuard |= (region.Protect & (PAGE_GUARD | PAGE_READWRITE)) == (PAGE_GUARD | PAGE_READWRITE);
            }
        }
        return Store(snapshot, totals, allocation, stackBase, hasGuard) ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
    }

    bool Store(Snapshot& snapshot, Totals& totals, Allocation allocation, std::uintptr_t stackBase,
               bool hasGuard) const noexcept {
        if (!allocation.committed) { return true; }
        if (snapshot.count == snapshot.allocations.size()) { return false; }
        if (allocation.base == stackBase) {
            allocation.kind = Kind::Stack;
            totals.stackCommit += allocation.committed;
        } else if (IsHeapBase(allocation.base, totals.heapCount)) {
            // Additional heap segments and large heap blocks may have a different allocation base.
            allocation.kind = Kind::HeapBase;
            totals.heapBaseCommit += allocation.committed;
        } else if (hasGuard && allocation.reserved) {
            allocation.kind = Kind::StackCandidate;
            totals.stackCandidateCommit += allocation.committed;
        } else {
            totals.otherPrivateCommit += allocation.committed;
        }
        totals.privateCommit += allocation.committed;
        snapshot.allocations[snapshot.count++] = allocation;
        return true;
    }

    bool IsHeapBase(std::uintptr_t base, DWORD heapCount) const noexcept {
        for (DWORD heap = 0; heap < heapCount; ++heap) {
            if (reinterpret_cast<std::uintptr_t>(workspace_->heaps[heap]) == base) { return true; }
        }
        return false;
    }

    static const char* KindName(Kind kind) noexcept {
        switch (kind) {
        case Kind::HeapBase: return "heap_base";
        case Kind::Stack: return "stack";
        case Kind::StackCandidate: return "stack_candidate";
        default: return "other";
        }
    }

    static void WriteChanges(std::ostream& output, const Snapshot& previous, const Snapshot& current) {
        std::size_t previousIndex = 0;
        std::size_t currentIndex = 0;
        bool wroteChange = false;
        while (previousIndex < previous.count || currentIndex < current.count) {
            const Allocation* before = previousIndex < previous.count ? &previous.allocations[previousIndex] : nullptr;
            const Allocation* after = currentIndex < current.count ? &current.allocations[currentIndex] : nullptr;
            if (before && after && before->base < after->base) { after = nullptr; }
            if (before && after && after->base < before->base) { before = nullptr; }
            if (before) { ++previousIndex; }
            if (after) { ++currentIndex; }
            if (before && after && before->committed == after->committed && before->kind == after->kind) { continue; }

            const auto* allocation = after ? after : before;
            if (!allocation) { break; }
            if (std::exchange(wroteChange, true)) { output << ','; }
            output << "{\"base\":" << allocation->base << ",\"kind\":\"" << KindName(allocation->kind)
                   << "\",\"before\":" << (before ? before->committed : 0)
                   << ",\"after\":" << (after ? after->committed : 0)
                   << ",\"reserved\":" << allocation->reserved << '}';
        }
    }

    Workspace* workspace_ = nullptr;
    int nextSnapshot_ = 0;
    bool hasPrevious_ = false;
};

} // namespace vispick::test
