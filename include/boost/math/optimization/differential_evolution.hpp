/*
 * Copyright Nick Thompson, 2024
 * Use, modification and distribution are subject to the
 * Boost Software License, Version 1.0. (See accompanying file
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#ifndef BOOST_MATH_OPTIMIZATION_DIFFERENTIAL_EVOLUTION_HPP
#define BOOST_MATH_OPTIMIZATION_DIFFERENTIAL_EVOLUTION_HPP
#include <algorithm>
#include <array>
#include <atomic>
#include <boost/math/optimization/detail/common.hpp>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <list>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace boost::math::optimization {

// Storn, R., Price, K. (1997). Differential evolution-a simple and efficient heuristic for global optimization over
// continuous spaces.
// Journal of global optimization, 11, 341-359.
// See:
// https://www.cp.eng.chula.ac.th/~prabhas//teaching/ec/ec2012/storn_price_de.pdf

// We provide the parameters in a struct-there are too many of them and they are too unwieldy to pass individually:
template <typename ArgumentContainer> struct differential_evolution_parameters {
  using Real = typename ArgumentContainer::value_type;
  ArgumentContainer lower_bounds;
  ArgumentContainer upper_bounds;
  // mutation factor is also called scale factor or just F in the literature:
  Real mutation_factor = static_cast<Real>(0.65);
  double crossover_probability = 0.5;
  // Population in each generation:
  size_t NP = 500;
  size_t max_generations = 1000;
  ArgumentContainer const *initial_guess = nullptr;
  unsigned threads = std::thread::hardware_concurrency();
};

template <typename ArgumentContainer>
void validate_differential_evolution_parameters(differential_evolution_parameters<ArgumentContainer> const &de_params) {
  using std::isfinite;
  using std::isnan;
  std::ostringstream oss;
  detail::validate_bounds(de_params.lower_bounds, de_params.upper_bounds);
  if (de_params.NP < 4) {
    oss << __FILE__ << ":" << __LINE__ << ":" << __func__;
    oss << ": The population size must be at least 4, but requested population size of " << de_params.NP << ".";
    throw std::invalid_argument(oss.str());
  }
  // From: "Differential Evolution: A Practical Approach to Global Optimization (Natural Computing Series)"
  // > The scale factor, F in (0,1+), is a positive real number that controls the rate at which the population evolves.
  // > While there is no upper limit on F, effective values are seldom greater than 1.0.
  // ...
  // Also see "Limits on F", Section 2.5.1:
  // > This discontinuity at F = 1 reduces the number of mutants by half and can result in erratic convergence...
  auto F = de_params.mutation_factor;
  if (isnan(F) || F >= 1 || F <= 0) {
    oss << __FILE__ << ":" << __LINE__ << ":" << __func__;
    oss << ": F in (0, 1) is required, but got F=" << F << ".";
    throw std::domain_error(oss.str());
  }
  if (de_params.max_generations < 1) {
    oss << __FILE__ << ":" << __LINE__ << ":" << __func__;
    oss << ": There must be at least one generation.";
    throw std::invalid_argument(oss.str());
  }
  if (de_params.initial_guess) {
    detail::validate_initial_guess(*de_params.initial_guess, de_params.lower_bounds, de_params.upper_bounds);
  }
  if (de_params.threads == 0) {
    oss << __FILE__ << ":" << __LINE__ << ":" << __func__;
    oss << ": There must be at least one thread.";
    throw std::invalid_argument(oss.str());
  }
}

template <typename ArgumentContainer, class Func, class URBG>
ArgumentContainer differential_evolution(
    const Func cost_function, differential_evolution_parameters<ArgumentContainer> const &de_params, URBG &gen,
    std::invoke_result_t<Func, ArgumentContainer> target_value =
        std::numeric_limits<std::invoke_result_t<Func, ArgumentContainer>>::quiet_NaN(),
    std::atomic<bool> *cancellation = nullptr,
    std::vector<std::pair<ArgumentContainer, std::invoke_result_t<Func, ArgumentContainer>>> *queries = nullptr,
    std::atomic<std::invoke_result_t<Func, ArgumentContainer>> *current_minimum_cost = nullptr) {
  using Real = typename ArgumentContainer::value_type;
  using ResultType = std::invoke_result_t<Func, ArgumentContainer>;
  using std::clamp;
  using std::isnan;
  using std::round;
  using std::uniform_real_distribution;
  validate_differential_evolution_parameters(de_params);
  const size_t dimension = de_params.lower_bounds.size();
  auto NP = de_params.NP;
  auto population = detail::random_initial_population(de_params.lower_bounds, de_params.upper_bounds, NP, gen);
  if (de_params.initial_guess) {
    population[0] = *de_params.initial_guess;
  }
  std::vector<ResultType> cost(NP, std::numeric_limits<ResultType>::quiet_NaN());
  std::atomic<bool> target_attained = false;
  // This mutex is only used if the queries are stored:
  std::mutex mt;

  std::vector<std::thread> thread_pool;
  auto const threads = de_params.threads;
  for (size_t j = 0; j < threads; ++j) {
    // Note that if some members of the population take way longer to compute,
    // then this parallelization strategy is very suboptimal.
    // However, we tried using std::async (which should be robust to this particular problem),
    // but the overhead was just totally unacceptable on ARM Macs (the only platform tested).
    // As the economists say "there are no solutions, only tradeoffs".
    thread_pool.emplace_back([&, j]() {
      for (size_t i = j; i < cost.size(); i += threads) {
        cost[i] = cost_function(population[i]);
        if (current_minimum_cost && cost[i] < *current_minimum_cost) {
          *current_minimum_cost = cost[i];
        }
        if (queries) {
          std::scoped_lock lock(mt);
          queries->push_back(std::make_pair(population[i], cost[i]));
        }
        if (!isnan(target_value) && cost[i] <= target_value) {
          target_attained = true;
        }
      }
    });
  }
  for (auto &thread : thread_pool) {
    thread.join();
  }

  std::vector<ArgumentContainer> trial_vectors(NP);
  for (size_t i = 0; i < NP; ++i) {
    if constexpr (detail::has_resize_v<ArgumentContainer>) {
      trial_vectors[i].resize(dimension);
    }
  }

  for (size_t generation = 0; generation < de_params.max_generations; ++generation) {
    if (cancellation && *cancellation) {
      break;
    }
    if (target_attained) {
      break;
    }

    // You might be tempted to parallelize the generation of trial vectors.
    // Here's the deal: Reproducibly generating random numbers is a nightmare.
    // I first tried seeding thread-local random number generators with the global generator,
    // but ThreadSanitizer didn't like it. I *suspect* there's a way around this, but
    // even if it's formally threadsafe, there's still a lot of effort to get it computationally reproducible.
    uniform_real_distribution<Real> unif01(Real(0), Real(1));
    for (size_t i = 0; i < NP; ++i) {
      size_t r1, r2, r3;
      do {
        r1 = gen() % NP;
      } while (r1 == i);
      do {
        r2 = gen() % NP;
      } while (r2 == i || r2 == r1);
      do {
        r3 = gen() % NP;
      } while (r3 == i || r3 == r2 || r3 == r1);
      for (size_t j = 0; j < dimension; ++j) {
        // See equation (4) of the reference:
        auto guaranteed_changed_idx = gen() % dimension;
        if (unif01(gen) < de_params.crossover_probability || j == guaranteed_changed_idx) {
          auto tmp = population[r1][j] + de_params.mutation_factor * (population[r2][j] - population[r3][j]);
          auto const &lb = de_params.lower_bounds[j];
          auto const &ub = de_params.upper_bounds[j];
          // Some others recommend regenerating the indices rather than clamping;
          // I dunno seems like it could get stuck regenerating . . .
          trial_vectors[i][j] = clamp(tmp, lb, ub);
        } else {
          trial_vectors[i][j] = population[i][j];
        }
      }
    }

    thread_pool.resize(0);
    for (size_t j = 0; j < threads; ++j) {
      thread_pool.emplace_back([&, j]() {
        for (size_t i = j; i < cost.size(); i += threads) {
          if (target_attained) {
            return;
          }
          if (cancellation && *cancellation) {
            return;
          }
          auto const trial_cost = cost_function(trial_vectors[i]);
          if (isnan(trial_cost)) {
            continue;
          }
          if (queries) {
            std::scoped_lock lock(mt);
            queries->push_back(std::make_pair(trial_vectors[i], trial_cost));
          }
          if (trial_cost < cost[i] || isnan(cost[i])) {
            cost[i] = trial_cost;
            if (!isnan(target_value) && cost[i] <= target_value) {
              target_attained = true;
            }
            if (current_minimum_cost && cost[i] < *current_minimum_cost) {
              *current_minimum_cost = cost[i];
            }
            population[i] = trial_vectors[i];
          }
        }
      });
    }
    for (auto &thread : thread_pool) {
      thread.join();
    }
  }

  auto it = std::min_element(cost.begin(), cost.end());
  return population[std::distance(cost.begin(), it)];
}

} // namespace boost::math::optimization
#endif