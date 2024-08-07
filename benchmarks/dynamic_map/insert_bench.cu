/*
 * Copyright (c) 2023-2024, NVIDIA CORPORATION.
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

#include <benchmark_defaults.hpp>
#include <benchmark_utils.hpp>

#include <cuco/dynamic_map.cuh>
#include <cuco/utility/key_generator.cuh>

#include <nvbench/nvbench.cuh>

#include <thrust/device_vector.h>
#include <thrust/transform.h>

using namespace cuco::benchmark;  // defaults, dist_from_state
using namespace cuco::utility;    // key_generator, distribution

/**
 * @brief A benchmark evaluating `cuco::dynamic_map::insert` performance
 */
template <typename Key, typename Value, typename Dist>
std::enable_if_t<(sizeof(Key) == sizeof(Value)), void> dynamic_map_insert(
  nvbench::state& state, nvbench::type_list<Key, Value, Dist>)
{
  using pair_type = cuco::pair<Key, Value>;

  auto const num_keys     = state.get_int64("NumInputs");
  auto const initial_size = state.get_int64("InitSize");
  auto const batch_size   = state.get_int64("BatchSize");

  if (num_keys % batch_size) { state.skip("NumInputs must be divisible by BatchSize."); }

  thrust::device_vector<Key> keys(num_keys);

  key_generator gen;
  gen.generate(dist_from_state<Dist>(state), keys.begin(), keys.end());

  thrust::device_vector<pair_type> pairs(num_keys);
  thrust::transform(keys.begin(), keys.end(), pairs.begin(), [] __device__(Key const& key) {
    return pair_type(key, {});
  });

  state.add_element_count(num_keys);

  state.exec(
    nvbench::exec_tag::sync | nvbench::exec_tag::timer, [&](nvbench::launch& launch, auto& timer) {
      cuco::dynamic_map<Key, Value> map{static_cast<size_t>(initial_size),
                                        cuco::empty_key<Key>{-1},
                                        cuco::empty_value<Value>{-1},
                                        {},
                                        launch.get_stream()};

      timer.start();
      for (std::size_t i = 0; i < num_keys; i += batch_size) {
        map.insert(pairs.begin() + i, pairs.begin() + i + batch_size, {}, {}, launch.get_stream());
      }
      timer.stop();
    });
}

template <typename Key, typename Value, typename Dist>
std::enable_if_t<(sizeof(Key) != sizeof(Value)), void> dynamic_map_insert(
  nvbench::state& state, nvbench::type_list<Key, Value, Dist>)
{
  state.skip("Key should be the same type as Value.");
}

NVBENCH_BENCH_TYPES(dynamic_map_insert,
                    NVBENCH_TYPE_AXES(defaults::KEY_TYPE_RANGE,
                                      defaults::VALUE_TYPE_RANGE,
                                      nvbench::type_list<distribution::unique>))
  .set_name("dynamic_map_insert_unique_capacity")
  .set_type_axes_names({"Key", "Value", "Distribution"})
  .set_max_noise(defaults::MAX_NOISE)
  .add_int64_axis("NumInputs", defaults::N_RANGE)
  .add_int64_axis("InitSize", {defaults::INITIAL_SIZE})
  .add_int64_axis("BatchSize", {defaults::BATCH_SIZE});

NVBENCH_BENCH_TYPES(dynamic_map_insert,
                    NVBENCH_TYPE_AXES(defaults::KEY_TYPE_RANGE,
                                      defaults::VALUE_TYPE_RANGE,
                                      nvbench::type_list<distribution::uniform>))
  .set_name("dynamic_map_insert_uniform_multiplicity")
  .set_type_axes_names({"Key", "Value", "Distribution"})
  .set_max_noise(defaults::MAX_NOISE)
  .add_int64_axis("NumInputs", {defaults::N})
  .add_int64_axis("InitSize", {defaults::INITIAL_SIZE})
  .add_int64_axis("BatchSize", {defaults::BATCH_SIZE})
  .add_int64_axis("Multiplicity", defaults::MULTIPLICITY_RANGE);

NVBENCH_BENCH_TYPES(dynamic_map_insert,
                    NVBENCH_TYPE_AXES(defaults::KEY_TYPE_RANGE,
                                      defaults::VALUE_TYPE_RANGE,
                                      nvbench::type_list<distribution::gaussian>))
  .set_name("dynamic_map_insert_gaussian_skew")
  .set_type_axes_names({"Key", "Value", "Distribution"})
  .set_max_noise(defaults::MAX_NOISE)
  .add_int64_axis("NumInputs", {defaults::N})
  .add_int64_axis("InitSize", {defaults::INITIAL_SIZE})
  .add_int64_axis("BatchSize", {defaults::BATCH_SIZE})
  .add_float64_axis("Skew", defaults::SKEW_RANGE);