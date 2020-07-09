namespace cuco {



template <typename Key, typename Value, cuda::thread_scope Scope>
static_map<Key, Value, Scope>::static_map(std::size_t capacity, Key empty_key_sentinel, Value empty_value_sentinel) : 
  capacity_{capacity},
  empty_key_sentinel_{empty_key_sentinel},
  empty_value_sentinel_{empty_value_sentinel} {
    CUCO_CUDA_TRY(cudaMalloc(&slots_, capacity * sizeof(pair_atomic_type)));
    
    auto constexpr block_size = 256;
    auto constexpr stride = 4;
    auto const grid_size = (capacity + stride * block_size - 1) / (stride * block_size);
    initializeKernel
    <atomic_key_type, atomic_mapped_type>
    <<<grid_size, block_size>>>(slots_, empty_key_sentinel,
                                          empty_value_sentinel, capacity);

    CUCO_CUDA_TRY(cudaMallocManaged(&d_num_successes_, sizeof(atomic_ctr_type)));
  }



template <typename Key, typename Value, cuda::thread_scope Scope>
static_map<Key, Value, Scope>::~static_map() {
  CUCO_CUDA_TRY(cudaFree(slots_));
  CUCO_CUDA_TRY(cudaFree(d_num_successes_));
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename InputIt, typename Hash, typename KeyEqual>
void static_map<Key, Value, Scope>::insert(InputIt first, InputIt last, 
                                           Hash hash, KeyEqual key_equal) {

  auto num_keys = std::distance(first, last);
  auto const block_size = 128;
  auto const stride = 1;
  auto const tile_size = 8;
  auto const grid_size = (tile_size * num_keys + stride * block_size - 1) /
                          (stride * block_size);
  auto view = get_device_mutable_view();

  atomic_ctr_type h_num_successes{};
  *d_num_successes_ = 0;
  CUCO_CUDA_TRY(cudaMemPrefetchAsync(d_num_successes_, sizeof(atomic_ctr_type), 0));

  insertKernel<block_size, tile_size>
  <<<grid_size, block_size>>>(first, first + num_keys, d_num_successes_, view, 
                              hash, key_equal);
  CUCO_CUDA_TRY(cudaDeviceSynchronize());

  h_num_successes = d_num_successes_->load(cuda::std::memory_order_relaxed);
  size_ += h_num_successes;
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename InputIt, typename OutputIt, typename Hash, typename KeyEqual>
void static_map<Key, Value, Scope>::find(
                                    InputIt first, InputIt last, OutputIt output_begin, 
                                    Hash hash, KeyEqual key_equal) noexcept {
  auto num_keys = std::distance(first, last);
  auto const block_size = 128;
  auto const stride = 1;
  auto const tile_size = 4;
  auto const grid_size = (tile_size * num_keys + stride * block_size - 1) /
                          (stride * block_size);
  auto view = get_device_view();
  findKernel<tile_size><<<grid_size, block_size>>>(first, last, output_begin,
                                                    view, hash, key_equal);
  CUCO_CUDA_TRY(cudaDeviceSynchronize());    
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename InputIt, typename OutputIt, typename Hash, typename KeyEqual>
void static_map<Key, Value, Scope>::contains(
  InputIt first, InputIt last, OutputIt output_begin, Hash hash, KeyEqual key_equal) noexcept {
  
  auto num_keys = std::distance(first, last);
  auto const block_size = 128;
  auto const stride = 1;
  auto const tile_size = 4;
  auto const grid_size = (tile_size * num_keys + stride * block_size - 1) /
                          (stride * block_size);
  auto view = get_device_view();
  containsKernel<tile_size><<<grid_size, block_size>>>(first, last, output_begin,
                                                       view, hash, key_equal);
  CUCO_CUDA_TRY(cudaDeviceSynchronize());
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename Iterator, typename Hash, typename KeyEqual>
__device__ thrust::pair<Iterator, bool> static_map<Key, Value, Scope>::device_mutable_view::insert(
  value_type const& insert_pair, Hash hash, KeyEqual key_equal) noexcept {

  auto current_slot{initial_slot(insert_pair.first, hash)};

  while (true) {
    using cuda::std::memory_order_relaxed;
    auto expected_key = empty_key_sentinel_;
    auto expected_value = empty_value_sentinel_;
    auto& slot_key = current_slot->first;
    auto& slot_value = current_slot->second;

    bool key_success = slot_key.compare_exchange_strong(expected_key,
                                                        insert_pair.first,
                                                        memory_order_relaxed);
    bool value_success = slot_value.compare_exchange_strong(expected_value,
                                                            insert_pair.second,
                                                            memory_order_relaxed);

    if(key_success) {
      while(not value_success) {
        value_success = slot_value.compare_exchange_strong(expected_value = empty_value_sentinel_,
                                                            insert_pair.second,
                                                            memory_order_relaxed);
      }
      return thrust::make_pair(current_slot, true);
    }
    else if(value_success) {
      slot_value.store(empty_value_sentinel_, memory_order_relaxed);
    }
    
    // if the key was already inserted by another thread, than this instance is a
    // duplicate, so the insert fails
    if (key_equal(insert_pair.first, expected_key)) {
      return thrust::make_pair(current_slot, false);
    }
    
    // if we couldn't insert the key, but it wasn't a duplicate, then there must
    // have been some other key there, so we keep looking for a slot
    current_slot = next_slot(current_slot);
  }
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename CG, typename Iterator, typename Hash, typename KeyEqual>
__device__ thrust::pair<Iterator, bool> static_map<Key, Value, Scope>::device_mutable_view::insert(
  CG g, value_type const& insert_pair, Hash hash, KeyEqual key_equal) noexcept {

  auto current_slot = initial_slot(g, insert_pair.first, hash);
  
  while(true) {
    key_type const existing_key = current_slot->first;
    uint32_t existing = g.ballot(key_equal(existing_key, insert_pair.first));
    
    // the key we are trying to insert is already in the map, so we return
    // with failure to insert
    if(existing) {
      uint32_t src_lane = __ffs(existing) - 1;
      intptr_t res_slot = g.shfl(reinterpret_cast<intptr_t>(current_slot), src_lane);
      return thrust::make_pair(reinterpret_cast<Iterator>(res_slot), false);
    }
    
    uint32_t empty = g.ballot(existing_key == empty_key_sentinel_);

    // we found an empty slot, but not the key we are inserting, so this must
    // be an empty slot into which we can insert the key
    if(empty) {
      // the first lane in the group with an empty slot will attempt the insert
      insert_result status{insert_result::CONTINUE};
      uint32_t src_lane = __ffs(empty) - 1;

      if(g.thread_rank() == src_lane) {
        using cuda::std::memory_order_relaxed;
        auto expected_key = empty_key_sentinel_;
        auto expected_value = empty_value_sentinel_;
        auto& slot_key = current_slot->first;
        auto& slot_value = current_slot->second;

        bool key_success = slot_key.compare_exchange_strong(expected_key,
                                                            insert_pair.first,
                                                            memory_order_relaxed);
        bool value_success = slot_value.compare_exchange_strong(expected_value,
                                                                insert_pair.second,
                                                                memory_order_relaxed);
        
        if(key_success) {
          while(not value_success) {
            value_success = slot_value.compare_exchange_strong(expected_value = empty_value_sentinel_,
                                                                insert_pair.second,
                                                                memory_order_relaxed);
          }
          status = insert_result::SUCCESS;
        }
        else if(value_success) {
          slot_value.store(empty_value_sentinel_, memory_order_relaxed);
        }
        
        // our key was already present in the slot, so our key is a duplicate
        if(key_equal(insert_pair.first, expected_key)) {
          status = insert_result::DUPLICATE;
        }
        // another key was inserted in the slot we wanted to try
        // so we need to try the next empty slot in the window
      }

      uint32_t res_status = g.shfl(static_cast<uint32_t>(status), src_lane);
      status = static_cast<insert_result>(res_status);

      // successful insert
      if(status == insert_result::SUCCESS) {
        intptr_t res_slot = g.shfl(reinterpret_cast<intptr_t>(current_slot), src_lane);
        return thrust::make_pair(reinterpret_cast<Iterator>(res_slot), true);
      }
      // duplicate present during insert
      if(status == insert_result::DUPLICATE) {
        intptr_t res_slot = g.shfl(reinterpret_cast<intptr_t>(current_slot), src_lane);
        return thrust::make_pair(reinterpret_cast<Iterator>(res_slot), false);
      }
      // if we've gotten this far, a different key took our spot 
      // before we could insert. We need to retry the insert on the
      // same window
    }
    // if there are no empty slots in the current window,
    // we move onto the next window
    else {
      current_slot = next_slot(g, current_slot);
    }
  }
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename Hash, typename Iterator>
__device__ Iterator static_map<Key, Value, Scope>::device_mutable_view::initial_slot(
  Key const& k, Hash hash) const noexcept {

  return &slots_[hash(k) % capacity_];
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename CG, typename Hash, typename Iterator>
__device__ Iterator static_map<Key, Value, Scope>::device_mutable_view::initial_slot(
  CG g, Key const& k, Hash hash) const noexcept {

  return &slots_[(hash(k) + g.thread_rank()) % capacity_];
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename Iterator>
__device__ Iterator static_map<Key, Value, Scope>::device_mutable_view::next_slot(
  Iterator s) const noexcept {

  return (++s < end()) ? s : slots_;
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename CG, typename Iterator>
__device__ Iterator static_map<Key, Value, Scope>::device_mutable_view::next_slot(
  CG g, Iterator s) const noexcept {

  uint32_t index = s - slots_;
  return &slots_[(index + g.size()) % capacity_];
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename Iterator, typename Hash, typename KeyEqual>
__device__ Iterator static_map<Key, Value, Scope>::device_view::find(
  Key const& k, Hash hash, KeyEqual key_equal) noexcept {

  auto current_slot = initial_slot(k, hash);

  while (true) {
    auto const existing_key =
        current_slot->first.load(cuda::std::memory_order_relaxed);
    // Key exists, return iterator to location
    if (key_equal(existing_key, k)) {
      return current_slot;
    }

    // Key doesn't exist, return end()
    if (existing_key == empty_key_sentinel_) {
      return end();
    }

    current_slot = next_slot(current_slot);
  }
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename CG, typename Iterator, typename Hash, typename KeyEqual>
__device__ Iterator static_map<Key, Value, Scope>::device_view::find(
  CG g, Key const& k, Hash hash, KeyEqual key_equal) noexcept {
  
  auto current_slot = initial_slot(g, k, hash);

  while(true) {
    key_type const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);
    uint32_t existing = g.ballot(key_equal(existing_key, k));
    
    // the key we were searching for was found by one of the threads,
    // so we return an iterator to the entry
    if(existing) {
      uint32_t src_lane = __ffs(existing) - 1;
      intptr_t res_slot = g.shfl(reinterpret_cast<intptr_t>(current_slot), src_lane);
      return reinterpret_cast<Iterator>(res_slot);
    }
    
    // we found an empty slot, meaning that the key we're searching 
    // for isn't in this submap, so we should move onto the next one
    uint32_t empty = g.ballot(existing_key == empty_key_sentinel_);
    if(empty) {
      return end();
    }

    // otherwise, all slots in the current window are full with other keys,
    // so we move onto the next window in the current submap
    
    current_slot = next_slot(g, current_slot);
  }
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename Hash, typename KeyEqual>
__device__ bool static_map<Key, Value, Scope>::device_view::contains(
  Key const& k, Hash hash, KeyEqual key_equal) noexcept {

  auto current_slot = initial_slot(k, hash);

  while (true) {
    auto const existing_key =
        current_slot->first.load(cuda::std::memory_order_relaxed);
    
    if (key_equal(existing_key, k)) {
      return true;
    }

    if (existing_key == empty_key_sentinel_) {
      return false;
    }

    current_slot = next_slot(current_slot);
  }
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename CG, typename Hash, typename KeyEqual>
__device__ bool static_map<Key, Value, Scope>::device_view::contains(
  CG g, Key const& k, Hash hash, KeyEqual key_equal) noexcept {

  auto current_slot = initial_slot(g, k, hash);

  while(true) {
    key_type const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);
    uint32_t existing = g.ballot(key_equal(existing_key, k));
    
    // the key we were searching for was found by one of the threads,
    // so we return an iterator to the entry
    if(existing) {
      return true;
    }
    
    // we found an empty slot, meaning that the key we're searching 
    // for isn't in this submap, so we should move onto the next one
    uint32_t empty = g.ballot(existing_key == empty_key_sentinel_);
    if(empty) {
      return false;
    }

    // otherwise, all slots in the current window are full with other keys,
    // so we move onto the next window in the current submap
    current_slot = next_slot(current_slot);
  }
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename Hash, typename Iterator>
__device__ Iterator static_map<Key, Value, Scope>::device_view::initial_slot(
  Key const& k, Hash hash) const noexcept {

  return &slots_[hash(k) % capacity_];
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename CG, typename Hash, typename Iterator>
__device__ Iterator static_map<Key, Value, Scope>::device_view::initial_slot(
  CG g, Key const& k, Hash hash) const noexcept {

  return &slots_[(hash(k) + g.thread_rank()) % capacity_];
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename Iterator>
__device__ Iterator static_map<Key, Value, Scope>::device_view::next_slot(
  Iterator s) const noexcept {

  return (++s < end()) ? s : slots_;
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename CG, typename Iterator>
__device__ Iterator static_map<Key, Value, Scope>::device_view::next_slot(
  CG g, Iterator s) const noexcept {

  uint32_t index = s - slots_;
  return &slots_[(index + g.size()) % capacity_];
}



}