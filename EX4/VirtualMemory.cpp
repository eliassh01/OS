#include "VirtualMemory.h"
#include "PhysicalMemory.h"



/*
 * Initialize the virtual memory.
 */
void VMinitialize() {
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        PMwrite(i, 0);
    }
}

word_t evictPage(uint64_t cyclic_frame_address, uint64_t cyclic_page) {
    word_t value;
    PMread(cyclic_frame_address, &value);
    PMevict(value, cyclic_page);
    PMwrite(cyclic_frame_address, 0);
    return value;
}

long long int findMinCyclic(long long int distance) {
    if (NUM_PAGES - distance <= distance) {
        return NUM_PAGES - distance;
    }
    else {
        return distance;
    }
}

void reached_leaves(uint64_t parent, const uint64_t &page_address,
                    uint64_t cur_page, uint64_t *cyclic_frame_address,
                    uint64_t *cyclic_page, uint64_t *cyclic_distance) {
    auto distance = (long long) (cur_page - page_address);
    if (distance < 0) {
        distance = -distance;
    }
    if ((uint64_t) findMinCyclic(distance) > *cyclic_distance)
    {
        *cyclic_distance = findMinCyclic(distance);
        *cyclic_frame_address = parent;
        *cyclic_page = cur_page;
    }
}

int checkIfZero(word_t *cur_root_children, word_t cur_root,  uint64_t i) {
    PMread(cur_root * PAGE_SIZE + i, cur_root_children + i);
    if (cur_root_children[i] == 0) {
        return 1;
    }
    return 0;
}


word_t treeTraversal(word_t *found_frame, int depth, uint64_t parent, uint64_t *max_frame_index,
                     uint64_t page_address, uint64_t cur_page, word_t last_frame,
                     uint64_t* cyclic_frame_address, uint64_t* cyclic_page,
                     uint64_t* cyclic_distance)
{
    word_t cur_root_children[PAGE_SIZE];
    word_t cur_root = 0;
    int num_of_zeroes = 0;

    if (depth != 0){
        PMread(parent, &cur_root);
    }

    if (depth == TABLES_DEPTH){
        reached_leaves(parent, page_address, cur_page, cyclic_frame_address, cyclic_page, cyclic_distance);
        return *found_frame;
    }

    cur_page = cur_page << OFFSET_WIDTH;

    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        num_of_zeroes += checkIfZero(cur_root_children, cur_root, i);
        if ((uint64_t) cur_root_children[i] > *max_frame_index) {
            *max_frame_index = cur_root_children[i];
        }
    }

    if (cur_root != 0 && cur_root != last_frame) {
        if (num_of_zeroes == PAGE_SIZE) {
            *found_frame = cur_root;
            PMwrite(parent, 0);
            return *found_frame;
        }
    }

    for (uint64_t i = 0; i < PAGE_SIZE; ++i) {
        if (!cur_root_children[i]) {
            continue;
        }
        treeTraversal(found_frame, depth + 1, cur_root * PAGE_SIZE + i, max_frame_index,
                      page_address,cur_page + i, last_frame,
                       cyclic_frame_address, cyclic_page, cyclic_distance);

    }
    return *found_frame;
}


word_t findValidFrame(word_t last_address, uint64_t pageAddress) {
    uint64_t max_frame_index = 0;
    word_t found_frame = 0;

    uint64_t cyclic_frame_address = 0;
    uint64_t cyclic_page = 0;
    uint64_t cyclic_distance = 0;

    found_frame = treeTraversal(&found_frame, 0, 0, &max_frame_index, pageAddress,
                  0, last_address, &cyclic_frame_address, &cyclic_page, &cyclic_distance);

    if (found_frame) {
        return found_frame;
    }
    return max_frame_index + 1 < NUM_FRAMES ? max_frame_index + 1 : evictPage(cyclic_frame_address, cyclic_page);
}



/* Reads a word from the given virtual address
 * and puts its content in *value.
 *
 * returns 1 on success.
 * returns 0 on failure (if the address cannot be mapped to a physical
 * address for any reason)
 */
int VMread(uint64_t virtualAddress, word_t* value) {
    if (virtualAddress > VIRTUAL_MEMORY_SIZE - 1) {
        return 0;
    }
    word_t address = 0;
    word_t base = 0;
    word_t last_address = 0;
    // [101] [0001] [0110]
    for (int i = 0; i < TABLES_DEPTH; i++) {
        // page_index = index inside page (table).
        uint64_t page_index = (virtualAddress >> (TABLES_DEPTH-i) * OFFSET_WIDTH) % PAGE_SIZE;
        // address = the page (frame or table) number we want to enter.
        PMread(base + page_index, &address);
        if (address == 0) {
            word_t frame_found = findValidFrame(last_address, virtualAddress >> OFFSET_WIDTH);
            if (i  == TABLES_DEPTH - 1) {
                PMrestore(frame_found, virtualAddress >> OFFSET_WIDTH);
            }
            else {
                for (uint64_t j = 0; j < PAGE_SIZE; j++) {
                    PMwrite((frame_found * PAGE_SIZE) + j, 0);
                }
            }
            PMwrite(base + page_index, frame_found);
            address = frame_found;
        }
        base = PAGE_SIZE*address;
        last_address = address;
    }
    uint64_t physical_address = (address * PAGE_SIZE) + (virtualAddress % PAGE_SIZE);
    PMread(physical_address, value);
    return 1;
}

/* Writes a word to the given virtual address.
 *
 * returns 1 on success.
 * returns 0 on failure (if the address cannot be mapped to a physical
 * address for any reason)
 */
int VMwrite(uint64_t virtualAddress, word_t value) {
    if (virtualAddress > VIRTUAL_MEMORY_SIZE - 1) {
        return 0;
    }
    word_t address = 0;
    word_t base = 0;
    word_t last_address = 0;
    // [101] [0001] [0110]
    for (int i = 0; i < TABLES_DEPTH; i++) {
        // page_index = index inside page (table).
        uint64_t page_index = (virtualAddress >> (TABLES_DEPTH-i) * OFFSET_WIDTH) % PAGE_SIZE;
        // address = the page (frame or table) number we want to enter.
        PMread(base + page_index, &address);
        if (address == 0) {
            word_t frame_found = findValidFrame(last_address, virtualAddress >> OFFSET_WIDTH);
            if (i + 1 == TABLES_DEPTH) {
                PMrestore(frame_found, virtualAddress >> OFFSET_WIDTH);
            }
            else {
                for (uint64_t j = 0; j < PAGE_SIZE; j++) {
                    PMwrite((frame_found * PAGE_SIZE) + j, 0);
                }
            }
            PMwrite(base + page_index, frame_found);
            address = frame_found;
        }
        base = PAGE_SIZE*address;
        last_address = address;
    }
    uint64_t physical_address = (address * PAGE_SIZE) + (virtualAddress % PAGE_SIZE);
    PMwrite(physical_address, value);
    return 1;
}
