/*
 * Copyright (c) 2020, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <benchmark/benchmark.h>

#include "cuco/static_map.cuh"
#include "../nvtx3.hpp"
#include <simt/atomic>

#include <thrust/for_each.h>
#include <iostream>
#include <fstream>



/**
 * @brief Generates input sizes and hash table occupancies
 *
 */
static void SweepLoadSize(benchmark::internal::Benchmark* b) {
  for (auto occupancy = 40; occupancy <= 90; occupancy += 10) {
    for (auto size = 10'000'000; size <= 10'000'000; size *= 2) {
      b->Args({size, occupancy});
    }
  }
}



template <typename Key, typename Value>
static void BM_cuco_insert(::benchmark::State& state) {
  using map_type = cuco::static_map<Key, Value>;
  
  std::size_t num_keys = state.range(0);
  float occupancy = state.range(1) / float{100};
  std::size_t size = num_keys / occupancy;

  map_type map{size, -1, -1};
  auto view = map.get_device_mutable_view();

  std::vector<cuco::pair_type<int, int>> h_pairs ( num_keys );
  
  for(auto i = 0; i < num_keys; ++i) {
    h_pairs[i] = cuco::make_pair(i, i);
  }

  thrust::device_vector<cuco::pair_type<int, int>> d_pairs( h_pairs );

  for(auto _ : state) {
    state.ResumeTiming();
    state.PauseTiming();
    map_type map{size, -1, -1};
    auto view = map.get_device_mutable_view();
    state.ResumeTiming();

    map.insert(d_pairs.begin(), d_pairs.end());

    state.PauseTiming();
  }

  state.SetBytesProcessed((sizeof(Key) + sizeof(Value)) *
                          int64_t(state.iterations()) *
                          int64_t(state.range(0)));
}



template <typename Key, typename Value>
static void BM_cuco_search_all(::benchmark::State& state) {
  using map_type = cuco::static_map<Key, Value>;
  
  std::size_t num_keys = state.range(0);
  float occupancy = state.range(1) / float{100};
  std::size_t size = num_keys / occupancy;

  map_type map{size, -1, -1};
  auto view = map.get_device_mutable_view();


  std::vector<int> h_keys( num_keys );
  std::vector<int> h_values( num_keys );
  std::vector<cuco::pair_type<int, int>> h_pairs ( num_keys );
  std::vector<int> h_results (num_keys);
  
  for(auto i = 0; i < num_keys; ++i) {
    h_keys[i] = i;
    h_values[i] = i;
    h_pairs[i] = cuco::make_pair(i, i);
  }

  thrust::device_vector<int> d_keys( h_keys ); 
  thrust::device_vector<int> d_results( num_keys);
  thrust::device_vector<cuco::pair_type<int, int>> d_pairs( h_pairs );

  map.insert(d_pairs.begin(), d_pairs.end());
  
  for(auto _ : state) {
    map.find(d_keys.begin(), d_keys.end(), d_results.begin());
  }

  state.SetBytesProcessed((sizeof(Key) + sizeof(Value)) *
                          int64_t(state.iterations()) *
                          int64_t(state.range(0)));
}

BENCHMARK_TEMPLATE(BM_cuco_insert, int32_t, int32_t)
  ->Unit(benchmark::kMillisecond)
  ->Apply(SweepLoadSize);

BENCHMARK_TEMPLATE(BM_cuco_search_all, int32_t, int32_t)
  ->Unit(benchmark::kMillisecond)
  ->Apply(SweepLoadSize);